/*
** Focused test for the GC2 paranoia fixpoint oracle.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

#if !LJ_GC2_PARANOIA
#error "t-gc2-paranoia requires -DLJ_GC2_PARANOIA=1"
#endif

static void run_script(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "lua error");
    abort();
  }
}

static int paranoia_finalizer(lua_State *L)
{
  int status = luaL_dostring(L,
    "local t = {}\n"
    "for i = 1, 80 do t[i] = {i, 'finalizer'..i} end\n");
  if (status != LUA_OK)
    lua_pop(L, 1);
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  void *stray;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);

  run_script(L,
    "local keep = {}\n"
    "for i = 1, 600 do\n"
    "  local t = {}\n"
    "  for j = 1, 80 do t[j] = 'value-'..i..'-'..j end\n"
    "  t['hash'..i] = {i, tostring(i)}\n"
    "  keep[i] = t\n"
    "end\n"
    "keep.dumped = string.dump(assert(loadstring("
    "  'return function(x) return x * 11 end'))())\n"
    "keep.closure = assert(loadstring(keep.dumped))\n"
    "keep.co = coroutine.create(function()\n"
    "  local s = 0\n"
    "  for i = 1, 100 do s = s + i end\n"
    "  coroutine.yield(s)\n"
    "  return s\n"
    "end)\n"
    "assert(coroutine.resume(keep.co))\n"
    "keep.wk = setmetatable({}, {__mode='kv'})\n"
    "do local k, v = {}, {}; keep.wk[k] = v end\n"
    "_G.__gc2_keep = keep\n");
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, paranoia_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_setglobal(L, "__gc2_ud");

  lj_gc_fullgc(L);
  assert(lj_gc2_paranoia_legacy_diff(g) == 0);
  assert(lj_gc2_ssb_empty(g));
  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  while (lj_gc_step(L) <= 0)
    ;
  assert(lj_gc2_paranoia_legacy_diff(g) == 0);
  assert(lj_gc2_ssb_empty(g));

  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ssb_empty(g));
  stray = lj_arena_alloc(&tg->alloc, &tg->prng, 64, LJ_AF_TRAVERSABLE);
  assert(stray != NULL);
  assert(lj_gc2_paranoia_legacy_diff(g) == 1);
  lj_arena_free(&tg->alloc, stray, 64);
  assert(lj_gc2_paranoia_legacy_diff(g) == 0);

  lj_gc2_legacy_cycle_end(g);
  lua_close(L);

  printf("t-gc2-paranoia OK: fixpoint oracle and stale-mark diff verified\n");
  return 0;
}
