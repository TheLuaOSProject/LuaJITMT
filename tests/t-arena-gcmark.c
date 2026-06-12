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

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TValue *tv;
  GCtab *tab;
  GCstr *str;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = { t = { 1, 2, 3 }, s = string.rep('m', 22000) }\n"
    "collectgarbage('collect')\n") == LUA_OK);
  g = G(L);

  lua_getglobal(L, "keep");
  tv = L->top - 1;
  assert(tvistab(tv));
  tab = tabV(tv);
  assert(arena_marked(g, obj2gco(tab)));

  lua_getfield(L, -1, "s");
  tv = L->top - 1;
  assert(tvisstr(tv));
  str = strV(tv);
  assert(lj_arena_ishuge(lj_arena_of(str)));
  assert(arena_marked(g, obj2gco(str)));

  lua_close(L);
  printf("t-arena-gcmark OK: legacy marks mirrored into arena metadata\n");
  return 0;
}
