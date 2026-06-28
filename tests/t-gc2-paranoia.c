/*
** Focused test for the GC2 paranoia fixpoint oracle.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_GC2_PARANOIA
#error "t-gc2-paranoia requires -DLJ_GC2_PARANOIA=1"
#endif

static int paranoia_finalizer(lua_State *L)
{
  int status = luaL_dostring(L,
    "local t = {}\n"
    "for i = 1, 80 do t[i] = {i, 'finalizer'..i} end\n");
  if (status != LUA_OK)
    lua_pop(L, 1);
  return 0;
}

static void run_true_minor_cycle(lua_State *L, global_State *g, TGState *tg)
{
  uint32_t swept;
  lj_gc2_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 1);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 1);
  lj_gc2_scan_cycle_roots(g, L);
  assert(lj_gc2_mark_complete(g, L, 64, ~(uint32_t)0) == 1);
  lj_gc2_mark_to_weak(g);
  assert(lj_gc2_weak_complete(g, gcref(g->gc.weak),
			      LJ_GC2_WEAK_DRAIN_BATCH) == 1);
  lj_gc2_weak_to_sweep(g);
  lj_gc2_sweep_bridge_ready(g);
  do {
    swept = lj_gc2_test_sweep_owner_progress(g, tg, LJ_GC2_SWEEP_BATCH);
  } while (swept != 0);
  assert(!lj_gc2_sweep_pending(g));
  lj_gc2_cycle_to_idle(g);
}

static void test_minor_major_paranoia(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lj_gc2_set_generational(g, 1);
  lj_gc_fullgc(L);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 1);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 1);
  ljt_lua_dostring(L,
    "_G.__gc2_minor_live = {}\n"
    "for i = 1, 120 do __gc2_minor_live[i] = {i, 'live'..i} end\n"
    "for i = 1, 400 do local t = {i, 'dead'..i}; t[3] = {i} end\n");
  run_true_minor_cycle(L, g, tg);
  lj_gc_fullgc(L);
  assert(lj_gc2_test_paranoia_root_diff(g) == 0);
  lj_gc2_set_generational(g, 0);
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  void *stray, *extra_stray;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);

  ljt_lua_dostring(L,
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
  assert(lj_gc2_test_paranoia_root_diff(g) == 0);
  assert(lj_gc2_test_ssb_empty(g));
  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  while (lj_gc_step(L) <= 0)
    ;
  assert(lj_gc2_test_paranoia_root_diff(g) == 0);
  assert(lj_gc2_test_ssb_empty(g));

  lj_gc2_mark_begin(g);
  assert(lj_gc2_test_ssb_empty(g));
  stray = lj_arena_alloc(&tg->alloc, &tg->prng, 64, LJ_AF_TRAVERSABLE);
  assert(stray != NULL);
  assert(lj_gc2_test_paranoia_root_diff(g) == 1);
  lj_arena_free(&tg->alloc, stray, 64);
  assert(lj_gc2_test_paranoia_root_diff(g) == 0);

  lj_gc2_cycle_to_idle(g);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 3000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  extra_tg.cur_L = L;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  lj_gc2_mark_begin(g);
  extra_stray = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			       LJ_AF_TRAVERSABLE);
  assert(extra_stray != NULL);
  assert(lj_gc2_test_paranoia_root_diff(g) == 1);
  lj_arena_free(&extra_tg.alloc, extra_stray, 64);
  assert(lj_gc2_test_paranoia_root_diff(g) == 0);
  lj_gc2_cycle_to_idle(g);

  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
  test_minor_major_paranoia();

  printf("t-gc2-paranoia OK: fixpoint oracle and stale-mark diff verified\n");
  return 0;
}
