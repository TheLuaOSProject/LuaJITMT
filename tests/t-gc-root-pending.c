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

static int pending_after_main_contains(TGState *tg, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_tg_gcroot_pending_after_main_acq(tg);
       o != NULL;
       o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    assert(++n < 1000000u);
  }
  return 0;
}

static int after_main_contains(global_State *g, GCobj *needle)
{
  GCobj *main = obj2gco(mainthread_acq(g));
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_obj_gcw_acq(main); o != NULL; o = lj_obj_gcw_acq(o)) {
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
  GCtab *t, *t2;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);

  t = lj_tab_new(L, 0, 0);
  t2 = lj_tab_new(L, 0, 0);
  assert(pending_contains(tg, obj2gco(t)));
  assert(pending_contains(tg, obj2gco(t2)));
  assert(!root_contains(g, obj2gco(t)));
  assert(!root_contains(g, obj2gco(t2)));

  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(root_contains(g, obj2gco(t)));
  assert(root_contains(g, obj2gco(t2)));
}

static void test_after_main_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  lua_State *L1, *L2;
  GCudata *ud;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);

  L1 = lua_newthread(L);
  L2 = lua_newthread(L);
  assert(L1 != NULL && L2 != NULL);
  assert(pending_after_main_contains(tg, obj2gco(L1)));
  assert(pending_after_main_contains(tg, obj2gco(L2)));
  assert(!after_main_contains(g, obj2gco(L1)));
  assert(!after_main_contains(g, obj2gco(L2)));

  lua_newuserdata(L, 16);
  ud = udataV(L->top - 1);
  assert(pending_after_main_contains(tg, obj2gco(ud)));
  assert(!after_main_contains(g, obj2gco(ud)));

  assert(lj_gc_flush_root_pending(g) >= 3u);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(after_main_contains(g, obj2gco(L1)));
  assert(after_main_contains(g, obj2gco(L2)));
  assert(after_main_contains(g, obj2gco(ud)));
  lua_pop(L, 3);
}

static void test_fullgc_flush(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtab *t;
  assert(tg != NULL);
  (void)lj_gc_flush_root_pending(g);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);

  lua_newtable(L);
  t = tabV(L->top - 1);
  assert(pending_contains(tg, obj2gco(t)));
  assert(!root_contains(g, obj2gco(t)));

  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  assert(root_contains(g, obj2gco(t)));

  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_explicit_flush(L);
  test_after_main_flush(L);
  test_fullgc_flush(L);
  lua_close(L);
  return 0;
}
