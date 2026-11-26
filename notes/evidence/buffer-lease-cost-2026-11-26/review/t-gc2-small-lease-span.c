/* Minimum-sized admission must retain the whole allocation, while callers
** requesting a span still receive its complete conservative extent. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"

static void *alloc_raw(lua_State *L, size_t size)
{
  void *p = lj_mem_new_nothrow(L, size);
  assert(p != NULL);
  assert(!lj_arena_ishuge(lj_arena_of(p)));
  return p;
}

/* Use real allocations to place a boundary immediately after the body.
** Padding seeks a chosen bitmap offset; no allocator metadata is edited. */
static void *alloc_at_offset(lua_State *L, size_t size, uint32_t offset)
{
  size_t span = (size + LJ_CELL_SIZE - 1u) & ~(size_t)(LJ_CELL_SIZE - 1u);
  uint32_t i;
  for (i = 0; i < 2u * LJ_ARENA_CELLS; i++) {
    void *pad = alloc_raw(L, LJ_CELL_SIZE);
    if (((lj_arena_cellof(pad) + 1u) & 63u) == offset) {
      void *p = alloc_raw(L, size);
      void *guard = alloc_raw(L, LJ_CELL_SIZE);
      if ((lj_arena_cellof(p) & 63u) == offset &&
          lj_arena_of(p) == lj_arena_of(guard) &&
          (uintptr_t)guard == (uintptr_t)p + span)
        return p;
    }
  }
  assert(!"could not place adjacent allocations at the requested bitmap offset");
  return NULL;
}

static uint64_t readers(GCArena *a)
{
  return lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_COUNT_MASK;
}

static void check_minimum(lua_State *L, void *p, size_t minimum, size_t extent)
{
  LJGC2Lease bounded, full;
  GCArena *a = lj_arena_of(p);
  uint64_t before = readers(a);
  size_t span = ~(size_t)0;
  int expected = minimum != 0 && minimum <= extent;

  assert(lj_gc2_small_lease_try(G(L), p, minimum, 0, &bounded, NULL) == expected);
  assert(readers(a) == before + (uint64_t)expected);
  assert(lj_gc2_small_lease_try(G(L), p, minimum, 0, &full, &span) == expected);
  assert(span == (expected ? extent : 0));
  assert(readers(a) == before + 2u * (uint64_t)expected);
  lj_gc2_lease_release(&bounded);
  lj_gc2_lease_release(&full);
  assert(readers(a) == before);
}

static void check_extent(lua_State *L, size_t size, uint32_t offset)
{
  void *p = alloc_at_offset(L, size, offset);
  size_t extent = (size + LJ_CELL_SIZE - 1u) & ~(size_t)(LJ_CELL_SIZE - 1u);
  size_t minimum;

  /* Every byte-sized minimum covers rounding within a cell, and the first
  ** byte of the guard must be refused even at the next bitmap word. */
  for (minimum = 0; minimum <= extent + 1u; minimum++)
    check_minimum(L, p, minimum, extent);
  check_minimum(L, p, LJ_HUGE_THRESHOLD + 1u, extent);
}

static void check_retained_free(lua_State *L)
{
  const size_t size = 8192u;
  unsigned char *p = (unsigned char *)alloc_at_offset(L, size, 63u);
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  uint64_t before = readers(a);
  LJGC2Lease held, denied;
  size_t i, span = ~(size_t)0;

  memset(p, 0xa6, size);
  /* A one-byte request skips the span walk, but its admission must still
  ** retain every payload byte during the allocator's real free path. */
  assert(lj_gc2_small_lease_try(G(L), p, 1u, 0, &held, NULL));
  assert(readers(a) == before + 1u);
  lj_mem_free(G(L), p, size);
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  assert(!lj_gc2_small_lease_try(G(L), p, 1u, 0, &denied, NULL));
  lj_gc2_lease_release(&denied);
  assert(!lj_gc2_small_lease_try(G(L), p, 1u, 0, &denied, &span));
  assert(span == 0);
  lj_gc2_lease_release(&denied);
  for (i = 0; i < 128u; i++) {
    void *other = alloc_raw(L, 64u);
    assert(other != p);
    memset(other, 0x3c, 64u);
  }
  for (i = 0; i < size; i++)
    assert(p[i] == 0xa6);
  assert(readers(a) == before + 1u);

  lj_gc2_lease_release(&held);
  assert(readers(a) == before);
  lj_gc2_lease_release(&held);
  assert(readers(a) == before);
  /* Ordinary collection owns completion of the deferred free; do not submit
  ** a second free or inspect the released allocation afterwards. */
  lua_gc(L, LUA_GCCOLLECT, 0);
}

int main(void)
{
  static const struct {
    size_t size;
    uint32_t offset;
  } cases[] = {
    { 1u, 61u }, { 16u, 63u }, { 17u, 62u }, { 31u, 63u },
    { 32u, 0u }, { 47u, 62u }, { 48u, 63u }, { 65u, 60u },
    { 1024u, 0u }, { 1025u, 63u }, { 2049u, 62u }, { 8192u, 63u }
  };
  lua_State *L = luaL_newstate();
  size_t i;
  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    check_extent(L, cases[i].size, cases[i].offset);
  check_retained_free(L);
  lua_close(L);
  puts("t-gc2-small-lease-span OK: minimum bounds, full extents and retained free");
  return 0;
}
