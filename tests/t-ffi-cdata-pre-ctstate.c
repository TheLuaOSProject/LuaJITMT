/*
** Fixed predefined cdata must validate before CTState publication.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_arena.h"
#include "lj_gc2.h"

#if LJ_HASFFI
static void check_neighbor_publication(lua_State *L)
{
  GCcdata *first = NULL, *next = NULL;
  GCArena *a;
  LJGC2QueuedInfo info;
  uint32_t i, cell, marked;
  uint32_t cells = lj_arena_ncells(sizeof(GCcdata) + sizeof(int32_t));
  int valid;
  lua_gc(L, LUA_GCSTOP, 0);
  for (i = 0; i < 128; i++) {
    first = next;
    next = lj_cdata_new_(L, CTID_INT32, sizeof(int32_t));
    if (first && lj_arena_of(first) == lj_arena_of(next) &&
        lj_arena_cellof(first) + cells == lj_arena_cellof(next)) break;
  }
  assert(i < 128 && lj_gc2_smr_read_tracked_try(G(L)));
  a = lj_arena_of(next);
  cell = lj_arena_cellof(next);
  assert(lj_arena_cdata_get(a, cell) && lj_arena_bm_get(a->block, cell));
  marked = lj_arena_bm_get(a->mark, cell);
  /* Model the two observations made as a peer publishes this neighbor. Its
  ** coverage is visible, while an earlier read saw an unused start cell. */
  lj_arena_bm_clear(a->mark, cell);
  lj_arena_bm_clear(a->block, cell);
  valid = lj_gc2_obj_queued_brief_held(G(L), obj2gco(first), NULL, &info);
  lj_arena_bm_set(a->block, cell);
  if (marked) lj_arena_bm_set(a->mark, cell);
  lj_gc2_smr_read_leave(G(L));
  assert(valid && info.base == first && info.gct == (uint32_t)~LJ_TCDATA);
}
#endif

int main(void)
{
#if LJ_HASFFI
  lua_State *L = luaL_newstate();
  global_State *g;
  GCcdata *cd;
  void *base = NULL;
  GCSize size = 0;
  uint16_t flags, id;
  uint8_t marked;

  assert(L != NULL);
  g = G(L);
  assert(ctype_ctsG(g) == NULL);

  /* lj_cdata_new_ links the object immediately; GC root validation therefore
  ** exercises the bootstrap layout oracle before this function returns. */
  cd = lj_cdata_new_(L, CTID_INT32, 4);
  assert(cd != NULL);
  assert(lj_cdata_validate(g, cd, &base, &size) == 1);
  assert(base == cd);
  assert(size == sizeof(GCcdata) + 4u);
  setcdataV(L, L->top, cd);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;

  flags = cdata_flags_acq(cd);
  cdata_flags_rel(cd, (uint16_t)(flags ^ LJ_CDATA_SIZE_MASK));
  assert(lj_cdata_validate(g, cd, NULL, NULL) == 0);
  cdata_flags_rel(cd, flags);

  id = cd->ctypeid;
  cd->ctypeid = (uint16_t)(CTID_CTYPEID + 1u);
  assert(lj_cdata_validate(g, cd, NULL, NULL) == 0);
  cd->ctypeid = id;

  marked = la_load8_acq(&cd->marked);
  la_store8_rel(&cd->marked, (uint8_t)(marked | 0x80u));
  assert(lj_cdata_validate(g, cd, NULL, NULL) == 0);
  la_store8_rel(&cd->marked, marked);

  assert(lj_cdata_validate(g, cd, &base, &size) == 1);
  check_neighbor_publication(L);
  lua_close(L);
  printf("t-ffi-cdata-pre-ctstate OK: fixed bootstrap layout validated\n");
#else
  printf("t-ffi-cdata-pre-ctstate SKIP: FFI disabled\n");
#endif
  return 0;
}
