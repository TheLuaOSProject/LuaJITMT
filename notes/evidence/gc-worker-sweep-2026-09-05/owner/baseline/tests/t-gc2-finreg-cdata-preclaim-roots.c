/*
** Focused regression for GC2 cdata preclaim vector raw roots.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#if LJ_HASFFI
#include "lj_cdata.h"

static int empty_finalizer(lua_State *L)
{
  (void)L;
  return 0;
}

static void full_cycle(lua_State *L)
{
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
}
#endif

int main(void)
{
#if LJ_HASFFI
  lua_State *L = luaL_newstate();
  global_State *g;
  GCRef *objv;
  TValue *finv;
  GCobj *o;
  TValue fin;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "local ffi = require('ffi'); return ffi.new('int[1]')") == LUA_OK);
  assert(tviscdata(L->top - 1));
  g = G(L);
  o = obj2gco(cdataV(L->top - 1));
  lua_pushcfunction(L, empty_finalizer);
  assert(lj_gc2_test_finreg_cdata_preclaim(L, g, o, L->top - 1));

  objv = gc2_finreg_cdata_preclaim_objvec_acq(g);
  finv = gc2_finreg_cdata_preclaim_finvec_acq(g);
  assert(objv != NULL && finv != NULL);
  assert(gc2_finreg_cdata_preclaim_count_acq(g) == 1u);
  lua_settop(L, 0);  /* The preclaim entry is now the only semantic owner. */

  for (i = 0; i < 4; i++) {
    full_cycle(L);
    assert(gc2_finreg_cdata_preclaim_objvec_acq(g) == objv);
    assert(gc2_finreg_cdata_preclaim_finvec_acq(g) == finv);
    assert(lj_gc2_mem_registered(g, objv));
    assert(lj_gc2_mem_registered(g, finv));
  }
  assert(lj_gc2_test_finreg_cdata_preclaim_take(L, g, o, &fin));
  assert(tvisfunc(&fin));

  /* Fixed vector storage remains globally owned even when no slot is live. */
  for (i = 0; i < 4; i++) {
    full_cycle(L);
    assert(gc2_finreg_cdata_preclaim_objvec_acq(g) == objv);
    assert(gc2_finreg_cdata_preclaim_finvec_acq(g) == finv);
    assert(lj_gc2_mem_registered(g, objv));
    assert(lj_gc2_mem_registered(g, finv));
  }

  lua_close(L);
  printf("t-gc2-finreg-cdata-preclaim-roots OK: fixed vectors survived\n");
#else
  printf("t-gc2-finreg-cdata-preclaim-roots SKIP: FFI disabled\n");
#endif
  return 0;
}
