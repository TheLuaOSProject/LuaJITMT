/* The established rooted-retry hook first closes all leases, then really
** resizes/collects/moves the stack. A new real IDLE writer is paused only
** afterward, so the next attempt must rebase both scalar output addresses. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#if !defined(LJ_TAB_TEST_HELPERS) || !defined(LJ_GC2_TEST_HELPERS)
#error "stack retry requires table/GC2 helpers"
#endif

typedef struct PauseCtx { global_State *g; uint32_t done; } PauseCtx;
static PauseCtx pause_ctx;
static pthread_t worker;
static uint32_t retry_hits, scalar_sources, scalar_results, stack_moves;
static int do_end;
static void *reclaim_main(void *unused)
{
  UNUSED(unused);
  (void)lj_gc2_reclaim_retired(pause_ctx.g, lj_gc2_retire_epoch(pause_ctx.g)+1u);
  la_store32_rel(&pause_ctx.done, 1); return NULL;
}
static void observe_scalar(lua_State *L, GCtab *t, uint32_t stage)
{
  UNUSED(t);
  assert(gc2_smr_reclaiming_acq(G(L)) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(lj_gc2_test_idle_reclaim_paused() && !la_load32_acq(&pause_ctx.done));
  if (stage == LJ_TAB_SCALAR_TEST_SOURCE) scalar_sources++;
  if (stage == LJ_TAB_SCALAR_TEST_RESULT) scalar_results++;
}
static void grow_collect_pause(lua_State *L, GCtab *t, int reader)
{
  TValue *oldstack = tvref(L->stack);
  uint32_t i;
  struct timespec delay = {0, 1000000};
  assert(reader == LJ_TAB_ROOTED_READER_NEXT && retry_hits++ == 0);
  assert(gc2_smr_readers_acq(G(L)) == 0);
  assert((lj_arena_remote_active_acq(lj_arena_of(t)) & LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  lj_tab_resize(L, t, 128, 0);
  for (i = 0; i < 6 && tvref(L->stack) == oldstack; i++)
    lj_state_growstack(L, (int)(256u << i));
  assert(tvref(L->stack) != oldstack); stack_moves++;
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  assert(tv_rawload(L->base) == tv_rawload(L->base + 4));
  assert(t == tabV(L->base) && lj_tab_node_acq(t) == &G(L)->nilnode);
  assert(gc2_smr_readers_acq(G(L)) == 0);
  assert((lj_arena_remote_active_acq(lj_arena_of(t)) & LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  pause_ctx.g = G(L); pause_ctx.done = 0;
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&worker, NULL, reclaim_main, NULL) == 0);
  for (i = 0; i < 5000 && !lj_gc2_test_idle_reclaim_paused(); i++) {
    assert(!la_load32_acq(&pause_ctx.done)); nanosleep(&delay, NULL);
  }
  assert(lj_gc2_test_idle_reclaim_paused());
  lj_tab_test_set_scalar_rooted_try_hook(observe_scalar);
}

int main(int argc, char **argv)
{
  lua_State *L; uint32_t idx = 719, anchors, waits;
  unsigned ko, vo; int result; TValue beforek, beforev;
  assert(argc == 4); do_end = !strcmp(argv[1], "end");
  assert(do_end || !strcmp(argv[1], "found"));
  ko = (unsigned)strtoul(argv[2], NULL, 10); vo = (unsigned)strtoul(argv[3], NULL, 10);
  assert(ko < 4 && vo < 4 && ko != vo);
  L = luaL_newstate(); assert(L); luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  assert(lua_checkstack(L, 32));
  lua_createtable(L, 8, 0);
  lua_pushinteger(L, 11); lua_rawseti(L, 1, 1);
  if (do_end) lua_pushinteger(L, 1); else lua_pushnil(L);
  lua_pushinteger(L, 998); lua_pushinteger(L, 999); lua_pushvalue(L, 1);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  anchors = lj_tg_root_anchor_top_acq(L2TG(L)); waits = lj_tab_test_wait_l_calls();
  beforek = L->base[ko]; beforev = L->base[vo];
  lj_tab_test_set_rooted_reader_retry_hook(grow_collect_pause);
  /* The nonwaiting primitive must not steal or invoke the allocating hook. */
  assert(lj_tab_test_nextscalar_rooted_try(L, L->base, L->base+1,
           L->base+ko, L->base+vo, &idx) == -2);
  assert(retry_hits == 0 && idx == 719);
  assert(tv_rawload(L->base+ko) == tv_rawload(&beforek));
  assert(tv_rawload(L->base+vo) == tv_rawload(&beforev));
  alarm(6);
  result = lj_tab_next_rooted(L, L->base, L->base+1, L->base+ko, L->base+vo, &idx);
  alarm(0);
  assert(result == (do_end ? 0 : 1));
  assert(retry_hits == 1 && stack_moves == 1 && scalar_sources == 1 && scalar_results == 1);
  assert(lj_gc2_test_idle_reclaim_paused() && !la_load32_acq(&pause_ctx.done));
  assert(gc2_smr_reclaiming_acq(G(L)) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(gc2_smr_readers_acq(G(L)) == 0 && lj_tg_root_anchor_top_acq(L2TG(L)) == anchors);
  assert(lj_tab_test_wait_l_calls() == waits+1u);
  if (do_end) {
    assert(idx == 719);
    assert(tv_rawload(L->base+ko) == tv_rawload(&beforek));
    assert(tv_rawload(L->base+vo) == tv_rawload(&beforev));
  } else {
    assert(numberVnum(L->base+ko) == 1 && numberVnum(L->base+vo) == 11 && idx == 2);
  }
  lj_tab_test_set_scalar_rooted_try_hook(NULL);
  lj_gc2_test_idle_reclaim_release(); assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&pause_ctx.done));
  lua_close(L);
  printf("stack move then refused SMR passed: %s %u %u\n", argv[1], ko, vo);
  return 0;
}
