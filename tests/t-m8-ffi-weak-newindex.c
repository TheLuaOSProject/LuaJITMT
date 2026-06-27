/*
** Focused M8 test for FFI cdata table-valued __newindex weak barriers.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_tg.h"

static void flush_and_drain(global_State *g, TGState *tg)
{
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
}

#if LJ_HASFFI
static void test_ffi_weak_newindex_target_write_barrier(lua_State *L,
							global_State *g,
							TGState *tg)
{
  GCtab *weak, *val;
  uint64_t weak_vals0;

  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[struct lj_m8_ffi_weak_newindex { int x; };]]\n"
    "local weak = setmetatable({ old = {} }, { __mode = 'v' })\n"
    "local ct = ffi.metatype('struct lj_m8_ffi_weak_newindex',\n"
    "  { __newindex = weak })\n"
    "local obj = ct()\n"
    "local val = {}\n"
    "return weak, obj, val, function(o, v) o.late = v end\n") == LUA_OK);
  weak = tabV(L->top - 4);
  val = tabV(L->top - 2);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  lj_gc2_mark_to_weak(g);
  assert(lj_gc2_test_weak_drain(g, 1) == 1u);
  assert(lj_gc2_test_weak_drain(g, 1) == 0);
  weak_vals0 = gc2_weak_values_marked_acq(g);

  lua_pushvalue(L, 4);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_call(L, 2, 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(gc2_weak_values_marked_acq(g) == weak_vals0 + 1u);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);

  lua_getfield(L, 1, "late");
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == val);
  lua_pop(L, 1);

  lj_gc2_legacy_cycle_end(g);
}

static void test_ffi_newindex_target_parent_barrier(lua_State *L,
						    global_State *g,
						    TGState *tg)
{
  GCtab *target, *val;

  lua_settop(L, 0);
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[struct lj_m8_ffi_parent_newindex { int x; };]]\n"
    "local target = {}\n"
    "local ct = ffi.metatype('struct lj_m8_ffi_parent_newindex',\n"
    "  { __newindex = target })\n"
    "local obj = ct()\n"
    "local val = {}\n"
    "return target, obj, val, function(o, v) o.child = v end\n") == LUA_OK);
  target = tabV(L->top - 4);
  val = tabV(L->top - 2);

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(target)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);

  lua_pushvalue(L, 4);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_call(L, 2, 0);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  flush_and_drain(g, tg);

  lua_getfield(L, 1, "child");
  assert(tvistab(L->top - 1) && tabV(L->top - 1) == val);
  lua_pop(L, 1);

  lj_gc2_legacy_cycle_end(g);
}
#endif

int main(void)
{
#if LJ_HASFFI
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);

  test_ffi_weak_newindex_target_write_barrier(L, g, tg);
  test_ffi_newindex_target_parent_barrier(L, g, tg);
  lua_close(L);
  printf("t-m8-ffi-weak-newindex OK: cdata __newindex weak target barrier verified\n");
#else
  printf("t-m8-ffi-weak-newindex SKIP: FFI disabled\n");
#endif
  return 0;
}
