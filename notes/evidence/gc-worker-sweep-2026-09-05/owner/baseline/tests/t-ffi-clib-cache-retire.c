/*
** Focused regression test for CLibrary side-cache retirement through the
** generic GC2 exclusive-reclaimer transaction.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_clib.h"

static CLibCacheEntry *new_retired_entry(lua_State *L, uint64_t epoch)
{
  CLibCacheEntry *entry = lj_mem_newt(L, sizeof(*entry), CLibCacheEntry);
  lj_clib_cache_next_rel(entry, NULL);
  lj_clib_cache_retired_next_rel(entry, NULL);
  lj_clib_cache_retire_epoch_rel(entry, epoch);
  lj_clib_cache_name_rel(entry, NULL);
  setnilV(&entry->val);
  return entry;
}

static unsigned retired_count(global_State *g)
{
  CLibCacheEntry *entry;
  unsigned n = 0;
  for (entry = lj_clib_cache_retired_head_acq(g); entry != NULL;
       entry = lj_clib_cache_retired_next_acq(entry)) {
    assert(lj_gc2_mem_registered_known(g, entry));
    assert(++n <= 2u);
  }
  return n;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  CLibCacheEntry *head, *tail;
  void *expect = NULL;
  uint64_t epoch;

  assert(L != NULL);
  g = G(L);
  assert(lj_clib_cache_retired_head_acq(g) == NULL);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  epoch = lj_gc2_retire_epoch(g);
  assert(epoch != 0);

  head = new_retired_entry(L, epoch);
  tail = new_retired_entry(L, epoch);
  lj_clib_cache_retired_next_rel(head, tail);
  assert(gc2_clib_cache_retired_cas(g, &expect, head));

  /* The age-rejected pass must traverse and requeue the complete detached
  ** chain while holding the real generic TLS capability. */
  (void)lj_gc2_reclaim_retired(g, epoch);
  assert(retired_count(g) == 2u);

  assert(lj_gc2_reclaim_retired(g, epoch + 1u) >= 2u);
  assert(lj_clib_cache_retired_head_acq(g) == NULL);

  lua_close(L);
  printf("t-ffi-clib-cache-retire OK: generic exact-owner drain retains and reclaims the full cache chain\n");
  return 0;
}
