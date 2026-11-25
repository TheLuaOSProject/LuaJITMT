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
#include "lj_tg.h"
#include "lj_thr.h"

static atomic_int ready, enabled, requested, parked, release_peer, timeout_hit;
static _Atomic(TGState *) peer_tg;
static int pool, stop_pool;
static uint32_t grace_round;
static atomic_uint grace_signals;
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
static int hold_peer(lua_State *L)
{
  LJNativeFrame frame;
  lj_native_enter_l(L, &frame);
  atomic_store(&peer_tg, L2TG(L));
  atomic_store(&ready, 1);
  while (!atomic_load(&requested) && !atomic_load(&release_peer))
    pause_briefly();
  (void)lj_native_leave_l(L, &frame);
  atomic_store(&parked, 1);
  while (!atomic_load(&release_peer))
    pause_briefly();
  assert(strcmp(lua_tostring(L, 1), "live sweep peer") == 0);
  return 0;
}

static int before_actions(global_State *g, uint32_t actions)
{
  if (!atomic_load(&enabled) || (actions & target) != target || atomic_load(&requested))
    return 0;
  if (grace_round && (!gc2_sweep_bridge_ready_acq(g) || !gc2_sweep_grace_needed_acq(g) ||
      (actions & ~(LJ_GC2_HS_FLUSH_SSB | LJ_GC2_HS_SCAN_OWNER_ROOTS)) ||
      atomic_fetch_add(&grace_signals, 1) + 1 != grace_round))
    return 0;
  if (atomic_exchange(&requested, 1))
    return 0;
  if (pool) {
    TGState *self = lj_thr_get_tg();
    assert(self && self != g->main_tg && lj_tg_load_cur_L(self) == NULL);
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

uint32_t __real_lj_safepoint_handshake(global_State *, uint32_t);
uint32_t __wrap_lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
    (void)before_actions(g, actions);
  return __real_lj_safepoint_handshake(g, actions);
}

int __real_lj_safepoint_resumable_epoch_try(global_State *, uint32_t, int);
int __wrap_lj_safepoint_resumable_epoch_try(global_State *g, uint32_t actions,
                                             int start)
{
  int observed = start && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
    before_actions(g, actions);
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
  if (pool) {
    global_State *g = G(L);
    TGState *peer = atomic_load(&peer_tg);
    uint32_t actions = 0;
    unsigned i;
    /* Stop the public step at the real WEAK -> SWEEP edge. Only the pool can
    ** initiate the selected bridge action before this callback returns. */
    for (i = 0; i < 10000 && gc2_phase_acq(g) != LJ_GC2_SWEEP; i++)
      lua_gc(L, LUA_GCSTEP, 1);
    assert(i != 10000 && !gc2_sweep_bridge_ready_acq(g));
    lua_gc(L, LUA_GCSTOP, 0);
    assert(lj_gc2_workers_set_l(L, 2, &actions) == 1);
    assert((actions & LJ_GC2_HS_STOPREQ) == 0);
#ifdef LJ_TRACE_TEST_HELPERS
    if (pause_publish) {
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
      assert(lj_tg_reqmask_acq(peer) & target);
      assert(lj_safepoint_test_signal_resumed_poll_stores() == 1);
      assert(lj_tg_reqmask_acq(self) == 0 && lj_tg_poll_acq(self) == 0);
      assert(lj_safepoint_test_signal_consumed_clears() == 1);
    }
#endif
    while ((!atomic_load(&requested) || !(lj_tg_reqmask_acq(peer) & target)) &&
	   !atomic_load(&release_peer)) {
      (void)lj_safepoint_poll(L);
      pause_briefly();
    }
  }
  lua_gc(L, LUA_GCRESTART, 0);
  lj_gc_threshold_store(G(L), 0);
  lua_pushboolean(L, 1);
  return 1;
}

static int park_seen(lua_State *L)
{
  lua_pushboolean(L, atomic_load(&parked));
  return 1;
}

static int finish(lua_State *L)
{
  global_State *g = G(L);
  if (!atomic_load(&release_peer)) {
    assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
    assert(gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN);
    assert(gc2_jit_phase_gate_acq(g) == 0);
    assert(gc2_sweep_bridge_ready_acq(g) == (grace_round != 0));
    if (grace_round)
      assert(la_load32_acq(&g->gc2.sweep_grace_needed) == (LJ_GC2_GRACE_ACTIVE | grace_round));
    assert(gc2_hs_pending_acq(g) != 0 && !lj_tg_mark_active_acq(L2TG(L)));
    assert(lj_tg_alloc_black_acq(L2TG(L)));
    if (target == LJ_GC2_HS_RESET_ALLOC || target == LJ_GC2_HS_SCAN_ROOTS)
      assert(la_load32_acq(&L2TG(L)->alloc.prepare_epoch) == gc2_cycle_acq(g));
    if (target == LJ_GC2_HS_SCAN_ROOTS)
      assert(gc2_sweep_root_scanned_acq(g) == 2);
    if (pool) {
      TGState *peer = atomic_load(&peer_tg);
      uint64_t epoch = gc2_hs_epoch_acq(g);
      assert(gc2_hs_leader_acq(g) == LJ_SAFEPOINT_RESUMABLE_LEADER);
      assert(gc2_fixpoint_epoch_actions_acq(g) & target);
      assert(lj_tg_hs_epoch_ack_acq(L2TG(L)) == epoch);
      assert(lj_tg_hs_epoch_ack_acq(peer) != epoch);
      assert(lj_tg_reqmask_acq(peer) & target);
      lua_createtable(L, 2, 1);
      lua_pop(L, 1);
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
    "local hold, begin, parked, finish, late_attach = ...\n"
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th = require('threading')\n"
    "local peer = assert(th.spawn(function(hold) hold('live sweep peer') end, hold))\n"
    "while not begin() do th.sleep(0.001) end\n"
    "if late_attach then\n"
    "  local late = assert(th.spawn(function() return 17 end))\n"
    "  local ok, value = late:join(5)\n"
    "  assert(ok and value==17)\n"
    "end\n"
    "local keep = {}\n"
    "for i=1,100000 do keep[i%8+1]={i}; if parked() then break end end\n"
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
    if (!strcmp(action, "grace1") || !strcmp(action, "grace2")) {
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
  assert(pthread_create(&timer, NULL, watchdog, NULL) == 0);
  status = lua_pcall(L, 5, 0, 0);
  atomic_store(&release_peer, 1);
  assert(pthread_join(timer, NULL) == 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  else {
    assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
    assert(gc2_n_workers_acq(G(L)) == (pool && !stop_pool ? 2u : 0u));
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
