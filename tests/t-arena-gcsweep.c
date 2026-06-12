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
  GCfunc *fn, *deadchunk, *deadfn, *livefn, *deadcf, *finchunk, *finfn;
  GCfunc *bcfn, *hugefn;
  GCproto *deadpt, *deadfnpt;
  GCproto *bcpt, *hugept, *finpt;
  GCSize before_fn, deadfn_size, before_drop, deadpt_size, deadchunk_size;
  GCSize before_raw, before_cf, deadcf_size;
  GCSize before_bc, bcfn_size, bcpt_size, before_huge, hugefn_size;
  GCSize hugept_size;
  GCSize before_fin, finpt_size, finchunk_size, finfn_size;
  void *raw;
  LJHugeInfo hugehi;
  GCArena *fna, *arra;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = {}\n"
    "keep.f = function(x) return x + 1 end\n"
    "keep.dead = loadstring('return 42')\n"
    "keep.parent = loadstring('return function(x) return x + 7 end')\n"
    "keep.deadfn = keep.parent()\n"
    "keep.livefn = keep.parent()\n"
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
  deadchunk = fn;
  deadchunk_size = sizeLfunc((MSize)deadchunk->l.nupvalues);
  deadpt = funcproto(fn);
  deadpt_size = deadpt->sizept;
  assert(ptr_state(deadchunk) == 2);
  assert(ptr_state(deadpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadfn = funcV(tv);
  assert(isluafunc(deadfn));
  deadfn_size = sizeLfunc((MSize)deadfn->l.nupvalues);
  deadfnpt = funcproto(deadfn);
  assert(ptr_state(deadfn) == 2);
  assert(ptr_state(deadfnpt) == 2);
  L->top--;

  lua_getfield(L, -1, "livefn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  livefn = funcV(tv);
  assert(isluafunc(livefn));
  assert(funcproto(livefn) == deadfnpt);
  assert(ptr_state(livefn) == 2);
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

  before_fn = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfn");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fn - deadfn_size);
  assert((ptr_state(deadfn) & 2u) == 0);
  assert(ptr_state(deadfnpt) == 2);
  assert(ptr_state(livefn) == 2);

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
  assert(g->gc.total <= before_drop - deadpt_size - deadchunk_size);
  assert((ptr_state(deadchunk) & 2u) == 0);
  assert((ptr_state(deadpt) & 2u) == 0);
  assert(ptr_state(raw) == 1);

  lua_getfield(L, -1, "arr");
  lua_pushcclosure(L, noop_finalizer, 1);
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadcf = funcV(tv);
  assert(!isluafunc(deadcf));
  assert(deadcf->c.nupvalues == 1);
  deadcf_size = sizeCfunc((MSize)deadcf->c.nupvalues);
  assert(tvistab(&deadcf->c.upvalue[0]));
  assert(tabV(&deadcf->c.upvalue[0]) == arrtab);
  assert((lj_arena_of(deadcf)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(deadcf) == 2);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(ptr_state(deadcf) == 2);

  before_cf = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_cf - deadcf_size);
  assert((ptr_state(deadcf) & 2u) == 0);
  assert(ptr_state(arrtab) == 2);

  assert(luaL_dostring(L,
    "do\n"
    "  local f = assert(loadstring('return function(y) return y * 9 end'))()\n"
    "  keep.bcblob = string.dump(f)\n"
    "  keep.bcdead = assert(loadstring(keep.bcblob))\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "bcdead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  bcfn = funcV(tv);
  assert(isluafunc(bcfn));
  bcfn_size = sizeLfunc((MSize)bcfn->l.nupvalues);
  bcpt = funcproto(bcfn);
  bcpt_size = bcpt->sizept;
  assert((lj_arena_of(bcpt)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(bcfn) == 2);
  assert(ptr_state(bcpt) == 2);
  L->top--;

  before_bc = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "bcdead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_bc - bcpt_size - bcfn_size);
  assert((ptr_state(bcfn) & 2u) == 0);
  assert((ptr_state(bcpt) & 2u) == 0);

  assert(luaL_dostring(L,
    "do\n"
    "  local t = {'return function() local x = 0\\n'}\n"
    "  for i = 1, 6000 do t[#t+1] = 'x = x + 1\\n' end\n"
    "  t[#t+1] = 'return x end'\n"
    "  keep.huge = assert(loadstring(table.concat(t)))()\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "huge");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  hugefn = funcV(tv);
  assert(isluafunc(hugefn));
  hugefn_size = sizeLfunc((MSize)hugefn->l.nupvalues);
  hugept = funcproto(hugefn);
  hugept_size = hugept->sizept;
  assert(hugept_size > LJ_HUGE_THRESHOLD);
  assert(lj_arena_ishuge(lj_arena_of(hugept)));
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, &hugehi) == 1);
  assert(hugehi.size == hugept_size);
  assert((hugehi.flags & LJ_HUGEF_TRAVERSABLE) != 0);
  assert((hugehi.flags & LJ_HUGEF_MARK) != 0);
  assert(ptr_state(hugefn) == 2);
  L->top--;

  before_huge = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "huge");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_huge - hugept_size - hugefn_size);
  assert((ptr_state(hugefn) & 2u) == 0);
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, NULL) == 0);

  assert(luaL_dostring(L,
    "keep.deadfin = loadstring('return 43')\n"
    "keep.deadfinfn = keep.parent()\n") ==
	 LUA_OK);
  lua_getfield(L, -1, "deadfin");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  finchunk = fn;
  finchunk_size = sizeLfunc((MSize)finchunk->l.nupvalues);
  finpt = funcproto(fn);
  finpt_size = finpt->sizept;
  assert(ptr_state(finchunk) == 2);
  assert(ptr_state(finpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfinfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  finfn = funcV(tv);
  assert(isluafunc(finfn));
  finfn_size = sizeLfunc((MSize)finfn->l.nupvalues);
  assert(funcproto(finfn) == deadfnpt);
  assert(ptr_state(finfn) == 2);
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
  lua_setfield(L, -2, "deadfinfn");
  lua_pushnil(L);
  lua_setfield(L, -2, "ud");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fin - finpt_size - finchunk_size - finfn_size);
  assert((ptr_state(finchunk) & 2u) == 0);
  assert((ptr_state(finfn) & 2u) == 0);
  assert((ptr_state(finpt) & 2u) == 0);

  lua_close(L);
  printf("t-arena-gcsweep OK: traversable runtime sweep bridge verified\n");
  return 0;
}
