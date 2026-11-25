/* Ordinary allocations while a peer is suspended at a SWEEP boundary. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"

static atomic_int ready, enabled, requested, parked, release_peer, timeout_hit;
static _Atomic(TGState *) peer_tg;
static int pool, stop_pool, terminal, pool_close_hot;
static atomic_int pool_close_started, hot_alloc_seen;
static uint32_t target_phase = LJ_GC2_SWEEP;
static uint32_t grace_round;
static atomic_uint grace_signals;
static int restart_grace;
static atomic_uint fresh_graces;
static void *late_body;
static HugeTab *late_huge;
static uint32_t late_cycle;
static uint64_t late_epoch;
static int late_roots;
static atomic_int late_scan;
#ifdef LJ_TRACE_TEST_HELPERS
static int pause_publish;
static atomic_int pool_started, first_attempt_returned;
static uint64_t paused_epoch;
#endif
static uint32_t target = LJ_GC2_HS_DISABLE_BARRIER | LJ_GC2_HS_ALLOC_BLACK;

static void pause_briefly(void)
{
  const struct timespec delay = { 0, 1000000 };
  nanosleep(&delay, NULL);
}

/* The peer services earlier epochs from a real published native frame. It
** leaves that frame just before the selected action is signalled and keeps its
** Lua stack untouched inside a managed call until the allocating TG returns. */
static int hold_fresh_stack(lua_State *L)
{
  atomic_store(&parked, 1);
  while (!atomic_load(&release_peer)) {
    if (late_roots) {
      uint32_t actions = lj_tg_reqmask_acq(L2TG(L));
      /* Creating the coroutine can leave its first roots in our local
      ** buffer. Publish that suffix while withholding its new stack scan. */
      if ((actions & LJ_GC2_HS_FLUSH_SSB) &&
          !(actions & (LJ_GC2_HS_SCAN_ROOTS | LJ_GC2_HS_SCAN_OWNER_ROOTS)))
        (void)lj_safepoint_poll(L);
    }
    pause_briefly();
  }
  assert(strcmp(lua_tostring(L, 1), "live sweep peer") == 0);
  return 0;
}

static int hold_peer(lua_State *L)
{
  LJNativeFrame frame;
  lj_native_enter_l(L, &frame);
  atomic_store(&peer_tg, L2TG(L));
  atomic_store(&ready, 1);
  while (!atomic_load(&requested) && !atomic_load(&release_peer))
    pause_briefly();
  (void)lj_native_leave_l(L, &frame);
  if (late_roots) {
    lua_State *co = lua_newthread(L);
    lua_pushcfunction(co, hold_fresh_stack);
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    assert(lua_resume(co, 1) == 0);
    return 0;
  }
  return hold_fresh_stack(L);
}

static int before_actions(global_State *g, uint32_t actions)
{
  uint32_t phase = gc2_phase_acq(g);
  if (!atomic_load(&enabled))
    return 0;
  if (terminal && pool && phase == LJ_GC2_SWEEP &&
      actions == LJ_GC2_HS_FLUSH_SSB && gc2_sweep_table_close_gate_sc(g) &&
      !atomic_load(&requested)) {
    TGState *self = lj_thr_get_tg();
    assert(self && self != g->main_tg && lj_tg_load_cur_L(self) == NULL);
    atomic_store(&pool_close_started, 1);
    if (pool_close_hot)
      while (!atomic_load(&hot_alloc_seen) && !atomic_load(&release_peer))
	pause_briefly();
  }
  if (phase != target_phase || (actions & target) != target)
    return 0;
  if (terminal && (!gc2_sweep_table_close_gate_sc(g) || actions != target))
    return 0;
  if (late_roots) {
    if (atomic_load(&parked) && gc2_sweep_bridge_ready_acq(g) &&
        gc2_thread_scan_needscan_pending_acq(g))
      atomic_store(&late_scan, 1);
    return 0;
  }
  if (atomic_load(&requested))
    return 0;
  if (grace_round && (!gc2_sweep_bridge_ready_acq(g) || !gc2_sweep_grace_needed_acq(g) ||
      (actions & ~(LJ_GC2_HS_FLUSH_SSB | LJ_GC2_HS_SCAN_OWNER_ROOTS)) ||
      atomic_fetch_add(&grace_signals, 1) + 1 != grace_round))
    return 0;
  if (atomic_exchange(&requested, 1))
    return 0;
  if (pool) {
    /* The main TG can ACK the pool's final FLUSH and start IDLE policy. */
    if (terminal)
      assert(atomic_load(&pool_close_started));
    else {
      TGState *self = lj_thr_get_tg();
      assert(self && self != g->main_tg && lj_tg_load_cur_L(self) == NULL);
    }
  }
  while (!atomic_load(&parked) && !atomic_load(&release_peer))
    pause_briefly();
#ifdef LJ_TRACE_TEST_HELPERS
  if (pause_publish) {
    while (!atomic_load(&pool_started) && !atomic_load(&release_peer))
      pause_briefly();
    lj_safepoint_test_signal_pause_arm(g->main_tg);
  }
#endif
  return 1;
}

uint32_t __real_lj_str_gc2_sweep_step(global_State *, uint32_t);
uint32_t __wrap_lj_str_gc2_sweep_step(global_State *g, uint32_t limit)
{
  uint32_t n = __real_lj_str_gc2_sweep_step(g, limit);
  if (late_roots && atomic_load(&enabled) && gc2_sweep_bridge_ready_acq(g) &&
      !lj_str_gc2_sweep_pending(g) && !atomic_exchange(&requested, 1)) {
    while (!atomic_load(&parked) && !atomic_load(&release_peer))
      pause_briefly();
  }
  return n;
}

uint32_t __real_lj_safepoint_handshake(global_State *, uint32_t);
uint32_t __wrap_lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  (void)before_actions(g, actions);
  return __real_lj_safepoint_handshake(g, actions);
}

int __real_lj_safepoint_resumable_epoch_try(global_State *, uint32_t, int);
int __wrap_lj_safepoint_resumable_epoch_try(global_State *g, uint32_t actions,
                                             int start)
{
  int observed = start && before_actions(g, actions);
  unsigned fresh = atomic_load(&fresh_graces);
  if (start && fresh >= 1 && fresh <= LJ_GC2_GRACE_EPOCHS &&
      la_load32_acq(&g->gc2.sweep_grace_needed) == (LJ_GC2_GRACE_ACTIVE | fresh)) {
    assert(gc2_cycle_acq(g) == late_cycle);
    assert(gc2_hs_epoch_acq(g) >= late_epoch + fresh - 1);
    assert(lj_arena_hugetab_lookup(late_huge, late_body, NULL));
    atomic_fetch_add(&fresh_graces, 1);
  }
  int complete = __real_lj_safepoint_resumable_epoch_try(g, actions, start);
#ifdef LJ_TRACE_TEST_HELPERS
  if (pause_publish && observed)
    atomic_store(&first_attempt_returned, 1);
#else
  (void)observed;
#endif
  return complete;
}

static int begin(lua_State *L)
{
  if (!atomic_load(&ready)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  atomic_store(&enabled, 1);
  if (pool || terminal) {
    global_State *g = G(L);
    unsigned i;
    /* Stop the public step at the real WEAK -> SWEEP edge. */
    for (i = 0; i < 10000 && gc2_phase_acq(g) != LJ_GC2_SWEEP; i++)
      lua_gc(L, LUA_GCSTEP, 1);
    assert(i != 10000 && !gc2_sweep_bridge_ready_acq(g));
    if (terminal && (target & LJ_GC2_HS_ENABLE_BARRIER))
      lj_gc2_set_generational(g, 1);
    lua_gc(L, LUA_GCSTOP, 0);
  }
  if (pool) {
    TGState *peer = atomic_load(&peer_tg);
    uint32_t actions = 0;
    assert(lj_gc2_workers_set_l(L, 2, &actions) == 1);
    assert((actions & LJ_GC2_HS_STOPREQ) == 0);
    if (pool_close_hot) {
      /* The pool reached its actual FLUSH entry. Leave it poised there until
      ** the main has executed a batch of bytecode allocations. */
      while (!atomic_load(&pool_close_started) && !atomic_load(&release_peer)) {
	(void)lj_safepoint_poll(L);
	pause_briefly();
      }
      lua_pushboolean(L, 1);
      return 1;
    }
#ifdef LJ_TRACE_TEST_HELPERS
    if (pause_publish) {
      global_State *g = G(L);
      TGState *self = L2TG(L);
      atomic_store(&pool_started, 1);
      while (!lj_safepoint_test_signal_paused() && !atomic_load(&release_peer)) {
	/* RESET can start just after pool startup returns. Service it before
	** waiting to consume the deliberately incomplete SCAN_ROOTS signal. */
	if (target == LJ_GC2_HS_SCAN_ROOTS &&
	    (lj_tg_reqmask_acq(self) & LJ_GC2_HS_RESET_ALLOC))
	  (void)lj_safepoint_poll(L);
	pause_briefly();
      }
      assert(lj_tg_reqmask_acq(self) & target);
      assert(lj_tg_poll_acq(self) == 0);
      paused_epoch = gc2_hs_epoch_acq(g);
      assert(lj_safepoint_poll(L) & target);
      assert(lj_tg_hs_epoch_ack_acq(self) == paused_epoch);
      assert(lj_tg_reqmask_acq(self) == 0 && lj_tg_poll_acq(self) == 0);
      lj_safepoint_test_signal_pause_release();
      while (!atomic_load(&first_attempt_returned) && !atomic_load(&release_peer))
	pause_briefly();
      /* The original publish turn has stored poll, and the parked peer keeps
      ** this exact epoch live. The completed local action must let us run. */
      assert(gc2_hs_epoch_acq(g) == paused_epoch && gc2_hs_pending_acq(g) != 0);
      assert((lj_tg_reqmask_acq(peer) & target) == target);
      assert(lj_safepoint_test_signal_resumed_poll_stores() == 1);
      assert(lj_tg_reqmask_acq(self) == 0 && lj_tg_poll_acq(self) == 0);
      assert(lj_safepoint_test_signal_consumed_clears() == 1);
    }
#endif
    while ((!atomic_load(&requested) ||
	    (lj_tg_reqmask_acq(peer) & target) != target) &&
	   !atomic_load(&release_peer)) {
      (void)lj_safepoint_poll(L);
      pause_briefly();
    }
  } else if (terminal) {
    global_State *g = G(L);
    unsigned i;
    /* Finish the real sweep work. Drain-only turns leave the terminal close
    ** to the next bytecode allocation. */
    for (i = 0; i != 10000; i++) {
      (void)lj_gc2_resumable_epoch_continue(g);
      lj_gc2_sweep_prepare_bridge_boundary(g, NULL);
      if (!lj_gc2_worker_drain(g, LJ_GC2_SWEEP_BATCH) &&
	  gc2_sweep_bridge_ready_acq(g) && !lj_gc2_sweep_pending(g) &&
	  !gc2_sweep_grace_needed_acq(g) && !gc2_fixpoint_epoch_actions_acq(g))
	break;
    }
    assert(i != 10000 && gc2_phase_acq(g) == LJ_GC2_SWEEP);
  }
  lua_gc(L, LUA_GCRESTART, 0);
  lj_gc_threshold_store(G(L), 0);
  if (terminal)
    lj_gc_mt_threshold_store(G(L), 0);
  lua_pushboolean(L, 1);
  return 1;
}

static int park_seen(lua_State *L)
{
  int seen = late_roots ? atomic_load(&late_scan) : atomic_load(&parked);
  if (pool_close_hot) {
    TGState *peer = atomic_load(&peer_tg);
    atomic_store(&hot_alloc_seen, 1);
    seen = seen && (lj_tg_reqmask_acq(peer) & target) == target;
    if (!seen)
      pause_briefly();
  }
  lua_pushboolean(L, seen);
  return 1;
}

static int finish(lua_State *L)
{
  global_State *g = G(L);
  if (!atomic_load(&release_peer)) {
    assert(gc2_phase_acq(g) == target_phase);
    assert(gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN);
    assert(gc2_jit_phase_gate_acq(g) == 0);
    assert(gc2_sweep_bridge_ready_acq(g) == (grace_round != 0 || late_roots || terminal));
    if (terminal) {
      assert(gc2_sweep_table_close_gate_sc(g));
      assert(gc2_sweep_root_scanned_acq(g) == (target_phase == LJ_GC2_SWEEP));
    }
    if (late_roots) {
      assert(gc2_sweep_root_scanned_acq(g) == 1);
      assert(gc2_thread_scan_needscan_pending_acq(g) != 0);
    }
    if (grace_round)
      assert(la_load32_acq(&g->gc2.sweep_grace_needed) == (LJ_GC2_GRACE_ACTIVE | grace_round));
    if (restart_grace) {
      GCSize size = LJ_HUGE_THRESHOLD + LJ_CELL_SIZE;
      LJHugeInfo hi;
      late_huge = &L2TG(L)->huge;
      late_body = lj_mem_newgco_unlinked_nothrow(L, size);
      assert(late_body && lj_arena_ishuge(lj_arena_of(late_body)));
      late_cycle = gc2_cycle_acq(g);
      late_epoch = gc2_hs_epoch_acq(g);
      /* A logical free racing an unfinished constructor retains its mapping.
      ** Abandoning the constructor publishes a new physical retirement. */
      lj_mem_free(g, late_body, size);
      assert(lj_arena_hugetab_lookup(late_huge, late_body, &hi));
      assert(hi.flags & LJ_HUGEF_DEFER_FREE);
      assert(lj_mem_abandon_gco_unpublished(g, late_body) ==
             LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP);
      assert(la_load32_acq(&g->gc2.sweep_grace_needed) == 1);
      assert(la_load64_acq(&lj_arena_of(late_body)->hdr.retire_epoch) == late_epoch);
      atomic_store(&fresh_graces, 1);
    }
    assert(gc2_hs_pending_acq(g) != 0);
    assert(lj_tg_mark_active_acq(L2TG(L)) ==
	   (target_phase == LJ_GC2_IDLE && (target & LJ_GC2_HS_ENABLE_BARRIER)));
    assert(lj_tg_alloc_black_acq(L2TG(L)) == (target_phase == LJ_GC2_SWEEP));
    if (target == LJ_GC2_HS_RESET_ALLOC || target == LJ_GC2_HS_SCAN_ROOTS)
      assert(la_load32_acq(&L2TG(L)->alloc.prepare_epoch) == gc2_cycle_acq(g));
    if (target == LJ_GC2_HS_SCAN_ROOTS && !late_roots)
      assert(gc2_sweep_root_scanned_acq(g) == 2);
    if (pool || terminal) {
      TGState *peer = atomic_load(&peer_tg);
      uint64_t epoch = gc2_hs_epoch_acq(g);
      assert(gc2_hs_leader_acq(g) == LJ_SAFEPOINT_RESUMABLE_LEADER);
      assert((gc2_fixpoint_epoch_actions_acq(g) & target) == target);
      assert(lj_tg_hs_epoch_ack_acq(L2TG(L)) == epoch);
      assert(lj_tg_hs_epoch_ack_acq(peer) != epoch);
      assert((lj_tg_reqmask_acq(peer) & target) == target);
      lua_createtable(L, 2, 1);
      lua_pop(L, 1);
      if (terminal)
	assert(lua_gc(L, LUA_GCSTEP, 1) == 0);
      if (stop_pool) {
	uint32_t actions = 0;
	assert(lj_gc2_workers_set_l(L, 0, &actions) == 1);
	assert(actions == 0);
	assert(gc2_n_workers_acq(g) == 0 && gc2_worker_exited_acq(g) == 2);
	assert(gc2_hs_epoch_acq(g) == epoch && gc2_hs_pending_acq(g) == 1);
	assert(lj_tg_reqmask_acq(peer) & target);
      }
    }
  }
  lua_gc(L, LUA_GCSTOP, 0);
  atomic_store(&release_peer, 1);
  return 0;
}

static void *watchdog(void *unused)
{
  unsigned i;
  (void)unused;
  for (i = 0; i < 5000 && !atomic_load(&release_peer); i++)
    pause_briefly();
  if (!atomic_load(&release_peer)) {
    atomic_store(&timeout_hit, 1);
    atomic_store(&release_peer, 1);
  }
  return NULL;
}

int main(int argc, char **argv)
{
  static const char source[] =
    "local hold, begin, parked, finish, late_attach, terminal = ...\n"
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th = require('threading')\n"
    "local peer = assert(th.spawn(function(hold) hold('live sweep peer') end, hold))\n"
    "local keep = terminal and {}\n"
    "while not begin() do th.sleep(0.001) end\n"
    "if late_attach then\n"
    "  local late = assert(th.spawn(function() return 17 end))\n"
    "  local ok, value = late:join(5)\n"
    "  assert(ok and value==17)\n"
    "end\n"
    "if terminal then\n"
    "  for i=1,100000 do local t={}; if i%32==0 and parked() then break end end\n"
    "else\n"
    "  keep = {}\n"
    "  for i=1,100000 do keep[i%8+1]={i}; if parked() then break end end\n"
    "end\n"
    "assert(parked())\n"
    "for i=1,200 do keep[i]={answer=i+23} end\n"
    "finish()\n"
    "assert(peer:join(5))\n"
    "collectgarbage('collect')\n"
    "for i=1,200 do assert(keep[i].answer==i+23) end\n";
  lua_State *L = luaL_newstate();
  pthread_t timer;
  int status;
  if (argc >= 2) {
    const char *action = argv[1];
    pool = !strncmp(action, "pool-", 5);
    if (pool)
      action += 5;
    if (!strcmp(action, "close-flush")) {
      terminal = 1;
      pool_close_hot = pool;
      target = LJ_GC2_HS_FLUSH_SSB;
    } else if (!strcmp(action, "close-idle") || !strcmp(action, "close-gen")) {
      terminal = 1;
      target_phase = LJ_GC2_IDLE;
      target = LJ_GC2_HS_ALLOC_WHITE | (!strcmp(action, "close-gen") ?
	LJ_GC2_HS_ENABLE_BARRIER : LJ_GC2_HS_DISABLE_BARRIER);
    } else if (!strcmp(action, "late-roots")) {
      late_roots = 1;
      target = LJ_GC2_HS_SCAN_ROOTS;
    } else if (!strcmp(action, "restart-grace")) {
      restart_grace = 1;
      grace_round = LJ_GC2_GRACE_EPOCHS;
      target = LJ_GC2_HS_FLUSH_SSB;
    } else if (!strcmp(action, "grace1") || !strcmp(action, "grace2")) {
      grace_round = (uint32_t)(action[5] - '0');
      target = LJ_GC2_HS_FLUSH_SSB;
    } else if (!strcmp(action, "reset"))
      target = LJ_GC2_HS_RESET_ALLOC;
    else {
      assert(!strcmp(action, "roots"));
      target = LJ_GC2_HS_SCAN_ROOTS;
    }
    if (argc == 3) {
      assert(pool);
#ifdef LJ_TRACE_TEST_HELPERS
      pause_publish = !strcmp(argv[2], "publish");
      if (!pause_publish)
#endif
      {
	assert(!strcmp(argv[2], "stop"));
	stop_pool = 1;
      }
    } else {
      assert(argc == 2);
    }
  }
  assert(L);
  luaL_openlibs(L);
  assert(luaL_loadbuffer(L, source, sizeof(source)-1, "=sweep entry") == LUA_OK);
  lua_pushcfunction(L, hold_peer);
  lua_pushcfunction(L, begin);
  lua_pushcfunction(L, park_seen);
  lua_pushcfunction(L, finish);
#ifdef LJ_TRACE_TEST_HELPERS
  lua_pushboolean(L, pause_publish);
#else
  lua_pushboolean(L, 0);
#endif
  lua_pushboolean(L, terminal);
  assert(pthread_create(&timer, NULL, watchdog, NULL) == 0);
  status = lua_pcall(L, 6, 0, 0);
  atomic_store(&release_peer, 1);
  assert(pthread_join(timer, NULL) == 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  else {
    assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
    assert(gc2_n_workers_acq(G(L)) == (pool && !stop_pool ? 2u : 0u));
    if (terminal) {
      assert(!gc2_sweep_table_close_gate_sc(G(L)));
      assert(gc2_jit_phase_gate_acq(G(L)) != 0);
    }
    if (restart_grace) {
      assert(atomic_load(&fresh_graces) == LJ_GC2_GRACE_EPOCHS + 1);
      assert(!lj_arena_hugetab_lookup(late_huge, late_body, NULL));
    }
#ifdef LJ_TRACE_TEST_HELPERS
    if (pause_publish) {
      assert(lj_tg_hs_epoch_ack_acq(L2TG(L)) > paused_epoch);
      lj_safepoint_test_signal_pause_reset();
    }
#endif
  }
  lua_close(L);
  assert(status == LUA_OK);
  if (atomic_load(&timeout_hit)) {
    fprintf(stderr, "allocation waited for a parked owner on sweep action %#x\n", target);
    return 1;
  }
  return 0;
}
