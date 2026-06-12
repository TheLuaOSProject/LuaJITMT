/*
** Focused test for the runtime traversable arena sweep bridge.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_tg.h"

static uint32_t ptr_state(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_state(a, cell);
}

static int noop_finalizer(lua_State *L)
{
  (void)L;
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TValue *tv;
  GCtab *keep, *arrtab;
  GCfunc *fn;
  GCproto *deadpt;
  GCproto *finpt;
  GCSize before_drop, deadpt_size, before_raw, before_fin, finpt_size;
  void *raw;
  GCArena *fna, *arra;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = {}\n"
    "keep.f = function(x) return x + 1 end\n"
    "keep.dead = loadstring('return 42')\n"
    "keep.arr = {}\n"
    "for i = 1, 300 do keep.arr[i] = i end\n"
    "collectgarbage('collect')\n") == LUA_OK);

  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(tg->alloc.sweep_epoch != 0);

  lua_getglobal(L, "keep");
  tv = L->top - 1;
  assert(tvistab(tv));
  keep = tabV(tv);
  assert((lj_arena_of(keep)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(keep) == 2);
  assert(lj_arena_of(keep)->hdr.sweep_epoch == tg->alloc.sweep_epoch);

  lua_getfield(L, -1, "f");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  fna = lj_arena_of(fn);
  assert((fna->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(fn) == 2);
  assert(fna->hdr.sweep_epoch == tg->alloc.sweep_epoch);
  L->top--;

  lua_getfield(L, -1, "dead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  deadpt = funcproto(fn);
  deadpt_size = deadpt->sizept;
  assert(ptr_state(deadpt) == 2);
  L->top--;

  lua_getfield(L, -1, "arr");
  tv = L->top - 1;
  assert(tvistab(tv));
  arrtab = tabV(tv);
  assert(arrtab->asize > 0);
  assert(arrtab->colo <= 0);
  arra = lj_arena_of(tvref(arrtab->array));
  assert((arra->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(arra->hdr.sweep_epoch == 0);
  assert(ptr_state(tvref(arrtab->array)) == 3);
  L->top--;

  before_raw = g->gc.total;
  raw = lj_mem_newgco_raw(L, 64, LJ_AF_TRAVERSABLE);
  assert(g->gc.total == before_raw + 64);
  assert(ptr_state(raw) == 2);
  assert(lj_mem_freegco_defer(g, raw, 64) == 1);
  assert(g->gc.total == before_raw);
  assert(ptr_state(raw) == 2);

  before_drop = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "dead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_drop - deadpt_size);
  assert((ptr_state(deadpt) & 2u) == 0);
  assert(ptr_state(raw) == 1);

  assert(luaL_dostring(L, "keep.deadfin = loadstring('return 43')\n") ==
	 LUA_OK);
  lua_getfield(L, -1, "deadfin");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  finpt = funcproto(fn);
  finpt_size = finpt->sizept;
  assert(ptr_state(finpt) == 2);
  L->top--;

  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, noop_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "ud");

  before_fin = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfin");
  lua_pushnil(L);
  lua_setfield(L, -2, "ud");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fin - finpt_size);
  assert((ptr_state(finpt) & 2u) == 0);

  lua_close(L);
  printf("t-arena-gcsweep OK: traversable runtime sweep bridge verified\n");
  return 0;
}
