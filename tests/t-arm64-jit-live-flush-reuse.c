/*
** Native macOS ARM64 lifecycle contract for live trace flush, SMR grace and
** public trace-number reuse. This deliberately covers one exact integer
** BC_LOOP root at a time; certified JFUNCF entry is covered by its dedicated
** fixture, while unsupported JIT surfaces remain closed.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_mcode.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-live-flush-reuse requires the admitted ARM64 root gates"
#endif

#define WAIT_LIMIT 20000000u
#define FLUSH_ACTIONS (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)

typedef struct PeerBootstrap {
  lua_State *L;
  uint32_t attached;
  uint32_t tid;
  uint32_t done;
} PeerBootstrap;

typedef struct FlushPeer {
  lua_State *L;
  global_State *g;
  uint64_t epoch_before;
  uint64_t epoch_after;
  uint32_t tid;
  uint32_t ready;
  uint32_t go;
  uint32_t saw_postadmission;
  uint32_t saw_jit_base;
  uint32_t status;
  uint32_t clean_before_detach;
  uint32_t detached;
  uint32_t done;
} FlushPeer;

typedef struct FlushCoordinator {
  global_State *g;
  TGState *main_tg;
  FlushPeer *peer;
  uint64_t epoch_before;
  uint32_t saw_reqmask_before_poll;
  uint32_t saw_poll;
  uint32_t released_entry;
  uint32_t done;
} FlushCoordinator;

static void wait_nonzero(const uint32_t *word, const char *what)
{
  uint32_t i;
  for (i = 0; i < WAIT_LIMIT; i++) {
    if (la_load32_acq(word) != 0)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  fprintf(stderr, "ARM64 live-flush timeout: %s\n", what);
  assert(!"ARM64 live-flush wait timed out");
}

static void wait_for_postadmission(const char *what)
{
  uint32_t i;
  for (i = 0; i < WAIT_LIMIT; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  fprintf(stderr, "ARM64 live-flush timeout: %s\n", what);
  assert(!"ARM64 root entry did not reach post-admission pause");
}

static void wait_for_signal_pause(void)
{
  uint32_t i;
  for (i = 0; i < WAIT_LIMIT; i++) {
    if (lj_safepoint_test_signal_paused())
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 FLUSHJ signal did not pause after reqmask");
}

static void wait_for_main_poll(TGState *tg)
{
  uint32_t i;
  for (i = 0; i < WAIT_LIMIT; i++) {
    if (lj_tg_poll_acq(tg) != 0)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 FLUSHJ signal did not publish poll");
}

static void *bootstrap_peer(void *arg)
{
  PeerBootstrap *ctx = (PeerBootstrap *)arg;
  TGState *tg;
  assert(lj_threading_attach_wait(ctx->L));
  tg = lj_thr_get_tg();
  assert(tg != NULL && tg != G(ctx->L)->main_tg);
  la_store32_rel(&ctx->tid, lj_tg_tid_acq(tg));
  la_store32_rel(&ctx->attached, 1);
  lj_threading_detach(ctx->L, 1);
  assert(lj_thr_get_tg() == NULL);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void *flush_peer(void *arg)
{
  FlushPeer *ctx = (FlushPeer *)arg;
  TGState *tg;

  assert(lj_threading_attach_wait(ctx->L));
  tg = lj_thr_get_tg();
  assert(tg != NULL && tg != ctx->g->main_tg);
  la_store32_rel(&ctx->tid, lj_tg_tid_acq(tg));
  la_store32_rel(&ctx->ready, 1);
  wait_nonzero(&ctx->go, "flush peer release");
  wait_for_postadmission("flush peer publication");
  la_store32_rel(&ctx->saw_postadmission, 1);
  if (lj_tg_load_jit_base(ctx->g->main_tg) != NULL)
    la_store32_rel(&ctx->saw_jit_base, 1);
  assert(la_load32_acq(&ctx->saw_jit_base) == 1);
  assert(gc2_hs_epoch_acq(ctx->g) == ctx->epoch_before);

  ctx->status = (uint32_t)lj_trace_flushall_hs_noevent(ctx->L);
  ctx->epoch_after = gc2_hs_epoch_acq(ctx->g);
  assert(lj_tg_hs_epoch_ack_acq(tg) == ctx->epoch_after);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  la_store32_rel(&ctx->clean_before_detach, 1);
  lj_threading_detach(ctx->L, 1);
  assert(lj_thr_get_tg() == NULL);
  la_store32_rel(&ctx->detached, 1);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void *coordinate_flush(void *arg)
{
  FlushCoordinator *ctx = (FlushCoordinator *)arg;
  global_State *g = ctx->g;
  TGState *tg = ctx->main_tg;
  uint32_t peer_tid;

  wait_for_signal_pause();
  peer_tid = la_load32_acq(&ctx->peer->tid);
  assert(peer_tid != 0);
  assert(lj_trace_test_root_entry_paused() ==
	 LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  assert(gc2_hs_epoch_acq(g) == ctx->epoch_before + 1u);
  assert(gc2_hs_actions_acq(g) == FLUSH_ACTIONS);
  assert(gc2_hs_leader_acq(g) == peer_tid);
  assert(gc2_hs_pending_acq(g) >= 2u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == ctx->epoch_before);
  assert(lj_tg_reqmask_acq(tg) == FLUSH_ACTIONS);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) != NULL);
  la_store32_rel(&ctx->saw_reqmask_before_poll, 1);

  /* Complete the production reqmask->poll publication while the owner remains
  ** paused after its final admission check. Only native XPOLL can observe it. */
  lj_safepoint_test_signal_pause_release();
  wait_for_main_poll(tg);
  assert(lj_tg_reqmask_acq(tg) == FLUSH_ACTIONS);
  assert(lj_tg_load_jit_base(tg) != NULL);
  la_store32_rel(&ctx->saw_poll, 1);
  lj_trace_test_root_entry_release();
  la_store32_rel(&ctx->released_entry, 1);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 live-flush chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void call_loop(lua_State *L, const char *name, lua_Integer expected)
{
  void *saved_cframe = L->cframe;
  int top = lua_gettop(L);
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 20);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 live-flush call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  assert(lua_tointeger(L, -1) == expected);
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
  assert(L->cframe == saved_cframe);
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  int top = lua_gettop(L);
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
  return pt;
}

static GCtrace *expect_admitted_root(jit_State *J, GCproto *pt)
{
  GCtrace *T;
  const BCIns *pc;
  BCIns patched;
  TraceNo tr = proto_trace_acq(pt);
  assert(tr == 1);
  T = traceref_safe(J, tr);
  assert(T != NULL);
  assert(trace_runnable_acq(T, tr));
  assert(trace_traceno_acq(T) == tr);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == tr);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_startpt_acq(T) == pt);
  assert((la_load8_acq(&T->unused1) &
	  TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_szmcode_acq(T) != 0);
  pc = trace_startpc_acq(T);
  assert(pc != NULL);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP);
  assert((TraceNo)bc_d(patched) == tr);
  return T;
}

static GCtrace *retired_trace_find(jit_State *J, GCtrace *needle)
{
  GCtrace *T;
  for (T = trace_retired_head_acq(J); T != NULL;
	 T = trace_retired_next_acq(T))
    if (T == needle)
      return T;
  return NULL;
}

static MCodeRetire *active_mcode_find(jit_State *J, MCode *needle)
{
  MCodeRetire *ret;
  for (ret = mcode_active_head_acq(J); ret != NULL;
	 ret = mcode_retired_next_acq(ret))
    if (ret->area == needle)
      return ret;
  return NULL;
}

static MCodeRetire *retired_mcode_find(jit_State *J, MCode *needle)
{
  MCodeRetire *ret;
  for (ret = mcode_retired_head_acq(J); ret != NULL;
	 ret = mcode_retired_next_acq(ret))
    if (ret->area == needle)
      return ret;
  return NULL;
}

static void expect_single_native_exit(ExitNo exitno)
{
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == exitno);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == exitno);
}

static void expect_idle_reclaim_ready(global_State *g, jit_State *J)
{
  assert(g->gc.state == GCSpause);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == 0);
  assert(gc2_worker_active_acq(g) == 0);
  assert(gc2_n_workers_acq(g) == 0);
  assert(gc2_assist_active_acq(g) == 0);
  assert(gc2_weak_drain_active_acq(g) == 0);
  assert(gc2_weak_write_active_acq(g) == 0);
  assert(!lj_gc2_activation_reclaim_veto(g));
  assert(gc2_jit_phase_gate_acq(g) == 1);
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_OPEN);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(!lj_tg_any_jit_active(g));
  assert(jit_token_acq(g) == 0);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(gc2_recovery_failed_acq(g) == 0);
}

static void bootstrap_sticky_mt(lua_State *L, lua_State *peer_L)
{
  global_State *g = G(L);
  PeerBootstrap ctx = {peer_L, 0, 0, 0};
  pthread_t worker;

  assert(mt_active_acq(g) == 0);
  assert(mt_live_acq(g) == 0);
  assert(gc2_n_threads_acq(g) == 1);
  assert(!lj_trace_hasany(g));
  assert(pthread_create(&worker, NULL, bootstrap_peer, &ctx) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&ctx.attached) == 1);
  assert(la_load32_acq(&ctx.tid) != 0);
  assert(la_load32_acq(&ctx.done) == 1);
  assert(mt_active_acq(g) != 0);
  assert(mt_live_acq(g) == 0);
  assert(gc2_n_threads_acq(g) == 1);
  assert(lj_tg_reclaim_dead(g) == 1);
  assert(lj_tg_reclaim_dead(g) == 0);
  assert(!lj_trace_hasany(g));
}

int main(void)
{
  lua_State *L = luaL_newstate();
  lua_State *peer_L;
  global_State *g;
  jit_State *J;
  TGState *main_tg;
  GCproto *oldpt, *newpt;
  GCtrace *baseline_scratch, *oldT, *newT;
  const BCIns *oldpc;
  BCIns oldstart;
  MCode *oldarea;
  MCodeRetire *oldowner, *retired_owner;
  size_t oldarea_size, oldszall;
  uint64_t baseline_stamp, epoch0, flush_epoch, retire_stamp;
  void *saved_cframe;
  int32_t saved_vmstate;
  FlushPeer peer;
  FlushCoordinator coordinator;
  pthread_t peer_thread, coordinator_thread;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  J = G2J(g);
  main_tg = L2TG(L);
  assert(J != NULL && main_tg != NULL);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);

  /* Keep one reusable peer state rooted below every temporary main-stack
  ** operation. Its first attach creates sticky MT before any trace exists. */
  peer_L = lua_newthread(L);
  assert(peer_L != NULL && lua_gettop(L) == 1);
  bootstrap_sticky_mt(L, peer_L);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_live_flush_old(n) "
      "local i,x=0,0 "
      "while i<n do i=i+1 x=x+i end "
      "return x "
    "end "
    "function __arm64_live_flush_new(n) "
      "local i,x=0,0 "
      "while i<n do i=i+1 x=x+i end "
      "return x "
    "end");
  assert(lua_gettop(L) == 1);
  call_loop(L, "__arm64_live_flush_old", 210);
  oldpt = global_proto(L, "__arm64_live_flush_old");
  newpt = global_proto(L, "__arm64_live_flush_new");
  assert(oldpt != newpt);
  assert(proto_trace_acq(newpt) == 0);
  oldT = expect_admitted_root(J, oldpt);
  oldpc = trace_startpc_acq(oldT);
  oldstart = trace_startins_acq(oldT);
  assert(bc_op(oldstart) == BC_LOOP);
  oldarea = J->mcarea;
  oldszall = J->szallmcarea;
  assert(oldarea != NULL && oldszall != 0);
  oldowner = active_mcode_find(J, oldarea);
  assert(oldowner != NULL);
  assert(oldowner->retire_epoch == MCODE_RETIRE_EPOCH_ACTIVE);
  oldarea_size = oldowner->size;
  assert(oldarea_size != 0 && oldarea_size <= oldszall);
  assert(mcode_retired_head_acq(J) == NULL);
  /* Successful assembly retires its separate construction copy. Prove this
  ** baseline node is exactly one unpublished, nonsemantic scratch body rather
  ** than silently treating an unrelated live/retired trace as test noise. */
  baseline_scratch = trace_retired_head_acq(J);
  assert(baseline_scratch != NULL && baseline_scratch != oldT);
  assert(trace_retired_next_acq(baseline_scratch) == NULL);
  assert(trace_retired_unpublished_acq(baseline_scratch));
  assert(trace_traceno_acq(baseline_scratch) == 0);
  assert(trace_nextroot_acq(baseline_scratch) == 0);
  assert(trace_mcode_acq(baseline_scratch) == NULL);
  assert(trace_szmcode_acq(baseline_scratch) == 0);
  assert(trace_startpt_acq(baseline_scratch) == NULL);
  assert(trace_startpc_acq(baseline_scratch) == NULL);
  baseline_stamp = la_load64_acq(&baseline_scratch->retire_epoch);
  assert(baseline_stamp == gc2_hs_epoch_acq(g) + 1u);
  assert(retired_trace_find(J, oldT) == NULL);

  /* A restored interpreted tail must not immediately record over the body
  ** which the live FLUSHJ just retired. Reopen recording only after grace. */
  run_lua(L,
    "jit.opt.start('hotloop=1000000','hotexit=1000000','maxtrace=2')");
  assert(expect_admitted_root(J, oldpt) == oldT);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(gc2_n_threads_acq(g) == 1);
  assert(mt_live_acq(g) == 0);
  assert(mt_active_acq(g) != 0);
  epoch0 = gc2_hs_epoch_acq(g);
  assert(epoch0 != 0);
  /* The hotcount reset above completed the first generation after scratch
  ** retirement. Thus live FLUSHJ is its second real grace generation. */
  assert(baseline_stamp == epoch0);
  assert(lj_tg_hs_epoch_ack_acq(main_tg) == epoch0);
  assert(lj_tg_reqmask_acq(main_tg) == 0);
  assert(lj_tg_poll_acq(main_tg) == 0);
  assert(lj_tg_load_jit_base(main_tg) == NULL);

  lj_trace_test_reset_retention_stats();
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_safepoint_test_signal_pause_arm(main_tg);

  peer = (FlushPeer){peer_L, g, epoch0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  coordinator = (FlushCoordinator){g, main_tg, &peer, epoch0, 0, 0, 0, 0};
  assert(pthread_create(&peer_thread, NULL, flush_peer, &peer) == 0);
  wait_nonzero(&peer.ready, "registered flush peer attach");
  assert(la_load32_acq(&peer.tid) != 0);
  assert(gc2_n_threads_acq(g) == 2);
  assert(mt_live_acq(g) == 1);
  assert(pthread_create(&coordinator_thread, NULL, coordinate_flush,
		&coordinator) == 0);
  lj_trace_test_root_entry_pause(
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  la_store32_rel(&peer.go, 1);
  saved_cframe = L->cframe;
  saved_vmstate = lj_tg_vmstate_load_acq(main_tg);
  call_loop(L, "__arm64_live_flush_old", 210);
  assert(pthread_join(coordinator_thread, NULL) == 0);
  assert(pthread_join(peer_thread, NULL) == 0);

  assert(la_load32_acq(&peer.saw_postadmission) == 1);
  assert(la_load32_acq(&peer.saw_jit_base) == 1);
  assert(peer.status == 0);
  assert(la_load32_acq(&peer.clean_before_detach) == 1);
  assert(la_load32_acq(&peer.detached) == 1);
  assert(la_load32_acq(&peer.done) == 1);
  assert(la_load32_acq(&coordinator.saw_reqmask_before_poll) == 1);
  assert(la_load32_acq(&coordinator.saw_poll) == 1);
  assert(la_load32_acq(&coordinator.released_entry) == 1);
  assert(la_load32_acq(&coordinator.done) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  expect_single_native_exit(5);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(main_tg) == saved_vmstate);
  assert(lj_safepoint_test_signal_resumed_poll_stores() == 1);
  assert(lj_safepoint_test_signal_consumed_clears() == 1);
  assert(lj_safepoint_test_signal_clean_before_leaves() == 1);

  flush_epoch = gc2_hs_epoch_acq(g);
  assert(flush_epoch == epoch0 + 1u);
  assert(peer.epoch_after == flush_epoch);
  assert(lj_tg_hs_epoch_ack_acq(main_tg) == flush_epoch);
  assert(gc2_hs_actions_acq(g) == FLUSH_ACTIONS);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(main_tg) == 0);
  assert(lj_tg_poll_acq(main_tg) == 0);
  assert(lj_tg_load_jit_base(main_tg) == NULL);
  assert((lj_tg_flags_acq(main_tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  lj_safepoint_test_signal_pause_reset();
  assert(gc2_n_threads_acq(g) == 1);
  assert(mt_live_acq(g) == 0);
  assert(lj_tg_reclaim_dead(g) == 1);
  assert(lj_tg_reclaim_dead(g) == 0);

  /* Sticky MT retains the exact raw slot as a stale-exit recovery name. Its
  ** traceno is already non-runnable; nextroot privately records slot 1. */
  assert((BCIns)la_load32_acq((const uint32_t *)oldpc) == oldstart);
  assert(proto_trace_acq(oldpt) == 0);
  assert(traceref_safe(J, 1) == oldT);
  assert(!trace_runnable_acq(oldT, 1));
  assert(trace_traceno_acq(oldT) == 0);
  assert(trace_nextroot_acq(oldT) == 1);
  assert(trace_link_acq(oldT) == 0);
  assert(trace_nextside_acq(oldT) == 0);
  assert(trace_native_pins_acq(oldT) == 0);
  retire_stamp = la_load64_acq(&oldT->retire_epoch);
  assert(retire_stamp == flush_epoch + 1u);
  assert(retire_stamp - 1u == flush_epoch);
  assert(retired_trace_find(J, oldT) == oldT);
  assert(trace_retired_head_acq(J) == oldT);
  assert(trace_retired_next_acq(oldT) == NULL);
  assert(retired_trace_find(J, baseline_scratch) == NULL);
  assert(trace_mcode_acq(oldT) != NULL);

  assert(mcode_active_head_acq(J) == NULL);
  assert(J->mcarea == NULL && J->mctop == NULL && J->mcbot == NULL);
  assert(J->szmcarea == 0);
  assert(J->szallmcarea == oldszall);
  retired_owner = retired_mcode_find(J, oldarea);
  assert(retired_owner == oldowner);
  assert(retired_owner->size == oldarea_size);
  assert(retired_owner->retire_epoch == flush_epoch);
  assert(J->freetrace == 0);
  assert(lj_trace_test_slot_release_calls() == 0);
  assert(lj_trace_test_slot_release_clears() == 0);
  assert(lj_trace_test_findfree_calls() == 0);

  /* Two real completed handshake generations are the production SMR clock.
  ** E+1 is deliberately too young; E+2 reclaims trace then mcode in one normal
  ** IDLE transaction. Never dereference oldT/oldowner after the second call. */
  expect_idle_reclaim_ready(g, J);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH) == 1);
  assert(gc2_hs_epoch_acq(g) == flush_epoch + 1u);
  assert(traceref_safe(J, 1) == oldT);
  assert(retired_trace_find(J, oldT) == oldT);
  assert(trace_retired_head_acq(J) == oldT);
  assert(trace_retired_next_acq(oldT) == NULL);
  assert(retired_trace_find(J, baseline_scratch) == NULL);
  assert(retired_mcode_find(J, oldarea) == oldowner);
  assert(J->szallmcarea == oldszall);
  assert(J->trace_reclaim_epoch == flush_epoch + 1u);
  assert(J->mcode_reclaim_epoch == flush_epoch + 1u);
  assert(lj_trace_test_slot_release_calls() == 0);
  assert(lj_trace_test_slot_release_clears() == 0);

  expect_idle_reclaim_ready(g, J);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH) == 1);
  assert(gc2_hs_epoch_acq(g) == flush_epoch + LJ_FLUSH_EPOCHS);
  assert(traceref_safe(J, 1) == NULL);
  assert(trace_retired_head_acq(J) == NULL);
  assert(mcode_retired_head_acq(J) == NULL);
  assert(mcode_active_head_acq(J) == NULL);
  assert(J->szallmcarea == 0);
  assert(J->trace_reclaim_epoch == flush_epoch + LJ_FLUSH_EPOCHS);
  assert(J->mcode_reclaim_epoch == flush_epoch + LJ_FLUSH_EPOCHS);
  assert(J->freetrace == 1);
  assert(lj_trace_test_slot_release_calls() == 1);
  assert(lj_trace_test_slot_release_clears() == 1);
  assert(lj_trace_test_last_released() == 1);
  assert(lj_trace_test_findfree_calls() == 0);
  assert(lj_trace_test_findfree_reuses() == 0);
  assert(lj_trace_test_findfree_grows() == 0);

  run_lua(L,
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2')");
  call_loop(L, "__arm64_live_flush_new", 210);
  newT = expect_admitted_root(J, newpt);
  assert(trace_startpt_acq(newT) == newpt);
  assert(la_load64_acq(&newT->retire_epoch) == 0);
  assert(trace_native_pins_acq(newT) == 0);
  assert(proto_trace_acq(oldpt) == 0);
  assert(J->mcarea != NULL);
  assert(mcode_active_head_acq(J) != NULL);
  assert(mcode_retired_head_acq(J) == NULL);
  assert(lj_trace_test_findfree_calls() == 1);
  assert(lj_trace_test_findfree_reuses() == 1);
  assert(lj_trace_test_findfree_grows() == 0);
  assert(lj_trace_test_last_findfree() == 1);

  /* Execute the replacement independently of its recording call. This proves
  ** the reused public name resolves only to the new body and exits normally. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  call_loop(L, "__arm64_live_flush_new", 210);
  expect_single_native_exit(8);
  assert(expect_admitted_root(J, newpt) == newT);
  assert(lj_tg_load_jit_base(main_tg) == NULL);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(main_tg) == 0);
  assert(lj_tg_poll_acq(main_tg) == 0);

  lua_pop(L, 1);  /* Release the reusable peer-state root. */
  assert(lua_gettop(L) == 0);
  lua_close(L);
  puts("t-arm64-jit-live-flush-reuse OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-live-flush-reuse SKIP");
  return 0;
}

#endif
