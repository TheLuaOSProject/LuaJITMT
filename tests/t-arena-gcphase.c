/*
** Focused test for arena allocation color during classic GC phases.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_arena.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  void *p;
  uint32_t cell;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(luaL_dostring(L,
    "hold = {}\n"
    "for i=1,30000 do hold[i] = { i, tostring(i), i % 17 } end\n") == LUA_OK);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert(tg->alloc.alloc_black == 0);

  lua_gc(L, LUA_GCRESTART, 0);
  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  for (i = 0; i < 1000 && g->gc.state == GCSpause; i++)
    lj_gc_step(L);
  assert(g->gc.state != GCSpause);
  assert(tg->alloc.alloc_black == 1);

  p = lj_arena_alloc(&tg->alloc, &tg->prng, 64, 0);
  assert(p != NULL);
  cell = lj_arena_cellof(p);
  assert(lj_arena_state(lj_arena_of(p), cell) == 3);
  lj_arena_free(&tg->alloc, p, 64);

  g->gc.stepmul = 200;
  for (i = 0; i < 20000 && g->gc.state != GCSpause; i++)
    lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(tg->alloc.alloc_black == 0);

  lua_close(L);
  printf("t-arena-gcphase OK: allocation color follows classic GC phase\n");
  return 0;
}
