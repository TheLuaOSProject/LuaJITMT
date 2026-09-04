/*
** MARK-close ownership loss neither parks a mutator nor fabricates worker
** progress. The parked worker must retry the durable intent after a release
** which wakes only worker_active, as the production owner release does.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_thr.h"
#include "lj_tg.h"

#include "lib/test_sleep.h"

static TGState *audited_tg;
static uint32_t audit_waits;
static uint32_t audited_native_entries;
static uint32_t audited_retry_yields;

/* Observe the actual peer-wait entry points rather than enforce a timing
** threshold. These wrappers leave the called runtime behavior intact. */
extern void __real_lj_native_enter(TGState *tg);
extern uint32_t __real_lj_thr_retry_yield(lua_State *L);

void __wrap_lj_native_enter(TGState *tg)
{
  if (la_load32_acq(&audit_waits) && tg == audited_tg)
    (void)la_add32_rlx(&audited_native_entries, 1);
  __real_lj_native_enter(tg);
}

uint32_t __wrap_lj_thr_retry_yield(lua_State *L)
{
  if (la_load32_acq(&audit_waits) && lj_thr_get_tg() == audited_tg)
    (void)la_add32_rlx(&audited_retry_yields, 1);
  return __real_lj_thr_retry_yield(L);
}

static int wait_for_counter(global_State *g,
			    uint64_t (*load)(global_State *), uint64_t target)
{
  uint32_t i;
  for (i = 0; i < 5000; i++) {
    if (load(g) >= target)
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

static int wait_for_mark_close(global_State *g)
{
  uint32_t i;
  for (i = 0; i < 5000; i++) {
    if (gc2_phase_acq(g) != LJ_GC2_MARK)
      return 1;
    sleep_ns(1000000L);
  }
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *parent, *child;
  uint64_t parks, progress, closes;
  uint32_t expect = 0, cycle, i;
  int closed;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  audited_tg = tg;
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, 1, 1);
  lua_pushinteger(L, 123);
  lua_rawseti(L, 2, 1);

  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_cycle_leader_acq(g) == 0);
  cycle = gc2_cycle_acq(g);
  assert(lua_gc(L, LUA_GCRESTART, -1) == 0);

  /* Hold exactly the production ownership word, with no background worker
  ** yet present. This models a peer descheduled immediately after its CAS. */
  assert(gc2_worker_active_cas(g, &expect, 1));
  closes = gc2_mark_complete_runs_acq(g);
  la_store32_rel(&audit_waits, 1);
  assert(lj_gc2_mark_complete(g, L, 1, 1) == 0);
  assert(lj_gc2_step_explicit(L, 1) == 0);
  (void)lj_gc_step(L);  /* The actual allocation-triggered driver. */
  la_store32_rel(&audit_waits, 0);
  assert(la_load32_acq(&audited_native_entries) == 0);
  assert(la_load32_acq(&audited_retry_yields) == 0);
  assert(gc2_mark_complete_runs_acq(g) > closes);
  assert(gc2_mark_close_intent_acq(g) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_acq(g) == cycle);
  assert(gc2_worker_active_acq(g) == 1);

  /* Public drains must report no completed work while the same exact owner
  ** remains suspended. In the old path this returned one on every call. */
  for (i = 0; i < 32; i++) {
    assert(lj_gc2_worker_drain(g, 1) == 0);
    assert(gc2_mark_close_intent_acq(g) == 1);
    assert(gc2_worker_active_acq(g) == 1);
  }

  parks = gc2_worker_parks_acq(g);
  progress = gc2_worker_async_progress_acq(g);
  assert(lj_gc2_workers_set(g, 1));
  assert(gc2_worker_started_acq(g) == 1);
  /* Three observed park attempts prove the real worker leaves its drain loop
  ** and repeatedly retries the held-token case without fake progress. */
  assert(wait_for_counter(g, gc2_worker_parks_acq, parks + 3));
  assert(gc2_worker_async_progress_acq(g) == progress);
  assert(gc2_mark_close_intent_acq(g) == 1);
  assert(gc2_worker_active_acq(g) == 1);

  /* A pending request must also preserve the native scheduling opportunity
  ** granted between bounded close rounds. Hold its deadline open until the
  ** test explicitly releases it; an eager helper must not close this gate. */
  parks = gc2_worker_parks_acq(g);
  gc2_jit_mark_auto_yield_rel(g, 1);
  gc2_jit_mark_yield_until_ns_rel(g, UINT64_MAX);
  gc2_jit_phase_gate_rel(g, 1);
  assert(wait_for_counter(g, gc2_worker_parks_acq, parks + 3));
  assert(gc2_jit_phase_gate_acq(g) == 1);
  assert(gc2_mark_close_intent_acq(g) == 1);
  assert(gc2_worker_async_progress_acq(g) == progress);

  /* Publish the main stack's real native certificate before the peer scans
  ** it. Release only the owner word and its futex: no worker_wake notification
  ** or fresh close request may mask a missed retry of the existing intent. */
  lj_native_enter(tg);
  gc2_jit_phase_gate_rel(g, 0);
  gc2_jit_mark_yield_until_ns_rel(g, 0);
  gc2_worker_active_rel(g, 0);
  la_futex_wake(&g->gc2.worker_active, 0x7fffffff);
  closed = wait_for_mark_close(g);
  assert(lj_gc2_workers_set(g, 0));
  (void)lj_native_leave(L);
  if (!closed)
    fprintf(stderr, "MARK retry stalled: phase=%u intent=%u roots=%u "
	    "rounds=%llu marks=%llu parks=%llu progress=%llu\n",
	    gc2_phase_acq(g), gc2_mark_close_intent_acq(g),
	    gc2_mark_root_scanned_acq(g),
	    (unsigned long long)gc2_fixpoint_rounds_acq(g),
	    (unsigned long long)gc2_marks_this_round_acq(g),
	    (unsigned long long)gc2_worker_parks_acq(g),
	    (unsigned long long)gc2_worker_async_progress_acq(g));
  assert(closed);
  assert(gc2_worker_async_progress_acq(g) > progress);
  assert(gc2_cycle_acq(g) == cycle);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);

  /* The caller which lost MARK ownership has returned, yet its original
  ** request still closes and the complete ordinary cycle remains driveable. */
  for (i = 0; i < 200000 && gc2_phase_acq(g) != LJ_GC2_IDLE; i++)
    (void)lua_gc(L, LUA_GCSTEP, 1);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_rawgeti(L, 1, 1);
  assert(lua_rawequal(L, -1, 2));
  lua_rawgeti(L, -1, 1);
  assert(lua_tointeger(L, -1) == 123);
  lua_close(L);
  puts("t-gc2-mark-close-progress OK: no peer wait, no fake progress, retry completed");
  return 0;
}
