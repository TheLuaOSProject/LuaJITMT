/*
** Focused test for mirroring legacy GC marks into arena metadata.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_str.h"
#include "lj_tg.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#endif
#if LJ_HASJIT
#include "lj_dispatch.h"
#include "lj_jit.h"
#endif

static int arena_mem_marked(global_State *g, void *p)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert(p != NULL);
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    assert((tg->tg_flags & TGF_HUGETAB) != 0);
    assert(lj_arena_hugetab_lookup(&tg->huge, p, &hi) == 1);
    return (hi.flags & LJ_HUGEF_MARK) != 0;
  }
  cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  assert(lj_arena_bm_get(a->block, cell));
  return lj_arena_bm_get(a->mark, cell) != 0;
}

static void assert_arena_white(global_State *g, void *p)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert(tg->alloc.sweep_epoch != 0);
  assert(p != NULL);
  a = lj_arena_of(p);
  assert(!lj_arena_ishuge(a));
  assert((a->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(a->hdr.sweep_epoch == tg->alloc.sweep_epoch);
  cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  assert(lj_arena_state(a, cell) == 2);
}

static int arena_marked(global_State *g, GCobj *o)
{
  void *p = (void *)o;
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    if (cdataisv(cd))
      p = memcdatav(cd);
  }
#endif
  return arena_mem_marked(g, p);
}

#if LJ_HASJIT
static GCtrace *find_trace(global_State *g)
{
  jit_State *J = G2J(g);
  MSize i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL)
      return T;
  }
  return NULL;
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TValue *tv;
  GCtab *tab;
  GCtab *arrtab, *hashtab;
  GCstr *str;
  GCfunc *fn;
  GCupval *uv;
  lua_State *co;
#if LJ_HASFFI
  GCcdata *cd;
  CTState *cts;
#endif
  LJHugeInfo hi;
#if LJ_HASJIT
  GCtrace *trace;
#endif

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = { t = { 1, 2, 3 }, s = string.rep('m', 22000) }\n"
    "keep.arr = {}\n"
    "for i = 1, 300 do keep.arr[i] = i end\n"
    "keep.hash = {}\n"
    "for i = 1, 300 do keep.hash['k'..i] = i end\n"
#if LJ_HASFFI
    "local ffi = require('ffi')\n"
    "keep.vcd = ffi.new('char[?]', 64)\n"
    "do\n"
    "  local cd = ffi.gc(ffi.new('char[?]', 96), function(x) keep.fin = x end)\n"
    "end\n"
#endif
    "keep.co = coroutine.create(function()\n"
    "  local x = { 4, 5, 6 }\n"
    "  keep.f = function() return x end\n"
    "  coroutine.yield()\n"
    "end)\n"
    "assert(coroutine.resume(keep.co))\n"
#if LJ_HASJIT
    "if jit and jit.opt then jit.opt.start('hotloop=1') end\n"
    "local n = 0\n"
    "for r = 1, 4 do for i = 1, 100 do n = n + i end end\n"
    "keep.n = n\n"
#endif
    "collectgarbage('collect')\n") == LUA_OK);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(tg->mark_active == 0);

  lua_getglobal(L, "keep");
  tv = L->top - 1;
  assert(tvistab(tv));
  tab = tabV(tv);
  assert((lj_arena_of(tab)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert_arena_white(g, tab);
  assert(arena_mem_marked(g, lj_str_tabh_acq(g)));

  lua_getfield(L, -1, "arr");
  tv = L->top - 1;
  assert(tvistab(tv));
  arrtab = tabV(tv);
  assert(arrtab->asize > 0);
  assert(arrtab->colo <= 0);
  assert(arena_mem_marked(g, lj_tab_array_hdrw(lj_tab_array_acq(arrtab))));
  L->top--;

  lua_getfield(L, -1, "hash");
  tv = L->top - 1;
  assert(tvistab(tv));
  hashtab = tabV(tv);
  assert(hashtab->hmask > 0);
  assert(arena_mem_marked(g, lj_tab_node_hdrw(lj_tab_node_acq(hashtab))));
  L->top--;

  lua_getfield(L, -1, "s");
  tv = L->top - 1;
  assert(tvisstr(tv));
  str = strV(tv);
  assert(lj_arena_ishuge(lj_arena_of(str)));
  assert((tg->tg_flags & TGF_HUGETAB) != 0);
  assert(lj_arena_hugetab_lookup(&tg->huge, str, &hi) == 1);
  assert((hi.flags & LJ_HUGEF_TRAVERSABLE) == 0);
  assert(arena_marked(g, obj2gco(str)));
  L->top--;

#if LJ_HASFFI
  lua_getfield(L, -1, "vcd");
  tv = L->top - 1;
  assert(tviscdata(tv));
  cd = cdataV(tv);
  assert(cdataisv(cd));
  assert(arena_marked(g, obj2gco(cd)));
  L->top--;

  lua_getfield(L, -1, "fin");
  tv = L->top - 1;
  assert(tviscdata(tv));
  cd = cdataV(tv);
  assert(cdataisv(cd));
  assert(arena_marked(g, obj2gco(cd)));
  L->top--;

  cts = ctype_ctsG(g);
  assert(cts != NULL);
  assert(arena_mem_marked(g, cts));
  assert(arena_mem_marked(g, ctype_tabh_acq(cts)));
#endif

  lua_getfield(L, -1, "co");
  tv = L->top - 1;
  assert(tvisthread(tv));
  co = threadV(tv);
  assert(arena_mem_marked(g, tvref(co->stack)));
  L->top--;

  lua_getfield(L, -1, "f");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  assert(fn->l.nupvalues == 1);
  uv = gco2uv(gcref(fn->l.uvptr[0]));
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert((lj_arena_of(uv)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert_arena_white(g, uv);
  L->top--;

#if LJ_HASJIT
  trace = find_trace(g);
  assert(trace != NULL);
  assert((lj_arena_of(trace)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert_arena_white(g, trace);
#endif

  lua_close(L);
  printf("t-arena-gcmark OK: legacy marks mirrored into arena metadata\n");
  return 0;
}
