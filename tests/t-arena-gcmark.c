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
#include "lj_tg.h"
#if LJ_HASJIT
#include "lj_dispatch.h"
#include "lj_jit.h"
#endif

static int arena_marked(global_State *g, GCobj *o)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  a = lj_arena_of(o);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    assert((tg->tg_flags & TGF_HUGETAB) != 0);
    assert(lj_arena_hugetab_lookup(&tg->huge, o, &hi) == 1);
    return (hi.flags & LJ_HUGEF_MARK) != 0;
  }
  cell = lj_arena_cellof(o);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  assert(lj_arena_bm_get(a->block, cell));
  return lj_arena_bm_get(a->mark, cell) != 0;
}

#if LJ_HASJIT
static GCtrace *find_trace(global_State *g)
{
  jit_State *J = G2J(g);
  MSize i;
  for (i = 1; i < J->sizetrace; i++)
    if (gcref(J->trace[i]) != NULL)
      return (GCtrace *)gcref(J->trace[i]);
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
  GCstr *str;
  GCfunc *fn;
  GCupval *uv;
  LJHugeInfo hi;
#if LJ_HASJIT
  GCtrace *trace;
#endif

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = { t = { 1, 2, 3 }, s = string.rep('m', 22000) }\n"
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

  lua_getglobal(L, "keep");
  tv = L->top - 1;
  assert(tvistab(tv));
  tab = tabV(tv);
  assert((lj_arena_of(tab)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(arena_marked(g, obj2gco(tab)));

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

  lua_getfield(L, -1, "f");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  assert(fn->l.nupvalues == 1);
  uv = gco2uv(gcref(fn->l.uvptr[0]));
  assert(!uv->closed);
  assert((lj_arena_of(uv)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(arena_marked(g, obj2gco(uv)));
  L->top--;

#if LJ_HASJIT
  trace = find_trace(g);
  assert(trace != NULL);
  assert((lj_arena_of(trace)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(arena_marked(g, obj2gco(trace)));
#endif

  lua_close(L);
  printf("t-arena-gcmark OK: legacy marks mirrored into arena metadata\n");
  return 0;
}
