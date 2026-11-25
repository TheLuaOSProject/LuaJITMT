/* A transferred VM stack root can reuse a completed closure scan. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_func.h"
#include "lj_tg.h"

static ptrdiff_t queued(TGState *tg)
{
  return lj_tg_ssb_next_acq(tg) - lj_tg_ssb_base_acq(tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCfunc *old, *fresh;
  GCupval *cell;
  ptrdiff_t before;
  unsigned i;
  assert(L);
  g = G(L);
  tg = L2TG(L);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(luaL_loadstring(L, "return 'transferred closure'") == LUA_OK);
  old = funcV(L->top - 1);
  for (i = 0; i < 40000; i++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  for (i = 0; i < 10000 && !(gc2_phase_acq(g) == LJ_GC2_SWEEP &&
       gc2_sweep_bridge_ready_acq(g)); i++)
    lua_gc(L, LUA_GCSTEP, 1);
  assert(i < 10000);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load32_acq(&old->l.gc2_scan_cycle) == gc2_cycle_acq(g));
  (void)lj_gc2_flush_ssb(g, tg);
  before = queued(tg);
  for (i = 0; i < 16; i++)
    lj_gc_pubtvroot_vm(L, L->top - 1);
  assert(queued(tg) == before);

  fresh = lj_func_newL_gc(L, funcproto(old), &old->l);
  assert(la_load32_acq(&fresh->l.gc2_scan_cycle) == 0);
  setgcVraw(L->top++, obj2gco(fresh), LJ_TFUNC);
  before = queued(tg);
  lj_gc_pubtvroot_vm(L, L->top - 1);
  assert(queued(tg) > before);

  cell = lj_func_newuvcell(L);
  setgcVraw(L->top++, obj2gco(cell), LJ_TUPVAL);
  before = queued(tg);
  lj_gc_pubtvroot_vm(L, L->top - 1);
  assert(queued(tg) > before);

  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lua_gettop(L) == 3 && funcV(L->top - 2) == fresh);
  assert((GCupval *)gcV(L->top - 1) == cell);
  assert(cell->gct == (uint8_t)~LJ_TUPVAL);
  lua_pop(L, 1);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  assert(strcmp(lua_tostring(L, -1), "transferred closure") == 0);
  lua_close(L);
  puts("VM root transfer coalesces completed FUNC and queues fresh FUNC/UPVAL");
  return 0;
}
