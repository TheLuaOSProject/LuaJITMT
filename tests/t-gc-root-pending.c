/*
** Focused test for per-TG pending GC root publication.
*/

#include <assert.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_tab.h"
#include "lj_tg.h"

static int root_contains(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < 1000000u);
  }
  return 0;
}

static int pending_contains(TGState *tg, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_tg_gcroot_pending_acq(tg); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < 1000000u);
  }
  return 0;
}

static void test_explicit_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtab *t;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);

  t = lj_tab_new(L, 0, 0);
  assert(pending_contains(tg, obj2gco(t)));
  assert(!root_contains(g, obj2gco(t)));

  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(root_contains(g, obj2gco(t)));
}

static void test_fullgc_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtab *t;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);

  lua_newtable(L);
  t = tabV(L->top - 1);
  assert(pending_contains(tg, obj2gco(t)));
  assert(!root_contains(g, obj2gco(t)));

  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(root_contains(g, obj2gco(t)));

  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_explicit_flush(L);
  test_fullgc_flush(L);
  lua_close(L);
  return 0;
}
