/*
** Native macOS ARM64 phase-gate contract for the first admitted trace shape.
** Each subtest owns a fresh Lua universe and one exact integer BC_LOOP root.
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
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-phase-gates requires the exact first-loop ARM64 gates"
#endif

#define WAIT_NS 10000000000ull
#define CLOSE_FAST_NS 50000000ull

enum {
  R_I = REF_FIRST,
  R_X,
  R_I_NEXT,
  R_X_NEXT,
  R_N,
  R_PRECOND,
  R_LOOP,
  R_XPOLL,
  R_I_BODY,
  R_X_BODY,
  R_COND,
  R_I_PHI,
  R_X_PHI,
  R_RENAME_I,
  R_RENAME_X,
  R_END
};

static const IRRef expected_snaprefs[] = {
  R_I, R_I_NEXT, R_X_NEXT, R_N, R_PRECOND, R_LOOP,
  R_I_BODY, R_X_BODY, R_COND
};

typedef struct StrictLoop {
  lua_State *L;
  global_State *g;
  jit_State *J;
  TGState *tg;
  GCproto *pt;
  int func_index;
} StrictLoop;

typedef struct IdleReclaimCtx {
  global_State *g;
  uint64_t epoch;
  uint32_t reclaimed;
  uint32_t done;
} IdleReclaimCtx;

typedef enum GateCloseKind {
  GATE_CLOSE_MARK,
  GATE_CLOSE_SWEEP
} GateCloseKind;

typedef struct GateCloseCtx {
  global_State *g;
  TGState *tg;
  GateCloseKind kind;
  uint64_t epoch;
  uint64_t ack;
  uint64_t elapsed_ns;
  uint32_t saw_pause;
  uint32_t saw_jit_base_before;
  uint32_t saw_jit_base_after;
  uint32_t gate_closed;
  uint32_t phase_stable;
  uint32_t request_words_clean;
  uint32_t sweep_displaced;
  uint32_t failed;
  uint32_t done;
} GateCloseCtx;

typedef struct GateWatchdogCtx {
  GateCloseCtx *closer;
  uint32_t saw_pause;
  uint32_t timed_out;
  uint32_t released;
} GateWatchdogCtx;

static int before_deadline(uint64_t started)
{
  uint64_t now = lj_thr_now_ns();
  return now >= started && now - started < WAIT_NS;
}

static int wait_for_postadmission(void)
{
  uint64_t started = lj_thr_now_ns();
  while (before_deadline(started)) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION)
      return 1;
    (void)lj_thr_retry_yield(NULL);
  }
  return 0;
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 phase-gate chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void call_cached_loop(StrictLoop *loop, lua_Integer expected)
{
  lua_State *L = loop->L;
  void *saved_cframe = L->cframe;
  int status;

  lua_pushvalue(L, loop->func_index);
  lua_pushinteger(L, 20);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 phase-gate loop failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  assert(lua_tointeger(L, -1) == expected);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  assert(ir[ref].o == op);
  assert(ir[ref].t.irt == type);
  assert(ir[ref].op1 == op1);
  assert(ir[ref].op2 == op2);
}

static void expect_strict_ir(const GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  const IRRef one = REF_TRUE - 1u;
  IRRef ref, k;

  assert(trace_nins_acq(T) == R_END);
  assert(trace_nk_acq(T) == REF_TRUE - 1u);
  assert(ir[one].o == IR_KINT);
  assert(ir[one].t.irt == IRT_INT);
  assert(ir[one].i == 1);
  for (k = REF_TRUE; k <= REF_NIL; k++) {
    assert(ir[k].o == IR_KPRI);
    assert(ir[k].t.irt == (uint8_t)(REF_NIL-k));
    assert(ir[k].op12 == 0);
  }

  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, R_I, IR_SLOAD, IRT_INT|IRT_GUARD,
	    3, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_X, IR_SLOAD, IRT_INT|IRT_GUARD,
	    4, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_I_NEXT, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I, one);
  expect_ir(ir, R_X_NEXT, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I_NEXT, R_X);
  expect_ir(ir, R_N, IR_SLOAD, IRT_INT|IRT_GUARD,
	    2, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_PRECOND, IR_GT, IRT_INT|IRT_GUARD, R_N, R_I_NEXT);
  expect_ir(ir, R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, R_I_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I_NEXT, one);
  expect_ir(ir, R_X_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I_BODY, R_X_NEXT);
  expect_ir(ir, R_COND, IR_LT, IRT_INT|IRT_GUARD, R_I_BODY, R_N);
  expect_ir(ir, R_I_PHI, IR_PHI, IRT_INT, R_I_NEXT, R_I_BODY);
  expect_ir(ir, R_X_PHI, IR_PHI, IRT_INT, R_X_NEXT, R_X_BODY);
  expect_ir(ir, R_RENAME_I, IR_RENAME, IRT_NIL, R_I_NEXT, 5);
  expect_ir(ir, R_RENAME_X, IR_RENAME, IRT_NIL, R_X_NEXT, 5);

  for (ref = REF_BASE; ref < trace_nins_acq(T); ref++)
    assert(!ra_hasspill(ir[ref].s));
}

static void expect_strict_trace(StrictLoop *loop)
{
  jit_State *J = loop->J;
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *pc;
  SnapShot *snap;
  SnapNo sn;
  TraceNo traceno;

  assert(T != NULL);
  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == loop->pt);
  assert(trace_spadjust_acq(T) == 0);
  assert(trace_topslot_acq(T) == (MSize)loop->pt->framesize);
  assert((la_load8_acq(&T->unused1) & TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert(trace_mcode_acq(T) != NULL);

  pc = trace_startpc_acq(T);
  assert(pc != NULL);
  assert(bc_op(trace_startins_acq(T)) == BC_LOOP);
  assert(bc_op((BCIns)la_load32_acq((const uint32_t *)pc)) == BC_JLOOP);
  assert((TraceNo)bc_d((BCIns)la_load32_acq((const uint32_t *)pc)) == 1);
  assert(proto_trace_acq(loop->pt) == 1);

  expect_strict_ir(T);
  snap = trace_snap_acq(T);
  assert(trace_nsnap_acq(T) ==
	 (SnapNo)(sizeof(expected_snaprefs)/sizeof(expected_snaprefs[0])));
  for (sn = 0; sn < trace_nsnap_acq(T); sn++)
    assert(snap_ref_acq(&snap[sn]) == expected_snaprefs[sn]);

  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static StrictLoop strict_loop_new(void)
{
  StrictLoop loop;
  GCfunc *fn;
  uint32_t worker_actions = 0;

  loop.L = luaL_newstate();
  assert(loop.L != NULL);
  luaL_openlibs(loop.L);
  loop.g = G(loop.L);
  loop.J = L2J(loop.L);
  loop.tg = L2TG(loop.L);
  assert(loop.g != NULL && loop.J != NULL && loop.tg != NULL);
  assert(lj_gc2_workers_set_l(loop.L, 0, &worker_actions) == 1);
  assert(gc2_n_workers_acq(loop.g) == 0);

  run_lua(loop.L,
    "collectgarbage('stop')\n"
    "jit.flush(); jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2')\n"
    "function __arm64_phase_gate_loop(n)\n"
    "  local i,x=0,0\n"
    "  while i<n do i=i+1 x=x+i end\n"
    "  return x\n"
    "end\n");
  lua_getglobal(loop.L, "__arm64_phase_gate_loop");
  loop.func_index = lua_gettop(loop.L);
  assert(lua_isfunction(loop.L, loop.func_index));
  fn = funcV(loop.L->top - 1);
  assert(isluafunc(fn));
  loop.pt = funcproto(fn);

  call_cached_loop(&loop, 210);
  assert(gc2_phase_acq(loop.g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(loop.g) != 0);
  assert(lj_tg_load_jit_base(loop.tg) == NULL);
  expect_strict_trace(&loop);
  return loop;
}

static void *idle_reclaim_main(void *arg)
{
  IdleReclaimCtx *ctx = (IdleReclaimCtx *)arg;
  ctx->reclaimed = lj_gc2_reclaim_retired(ctx->g, ctx->epoch);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static int wait_for_idle_pause(IdleReclaimCtx *ctx)
{
  uint64_t started = lj_thr_now_ns();
  while (before_deadline(started)) {
    if (lj_gc2_test_idle_reclaim_paused())
      return 1;
    if (la_load32_acq(&ctx->done) != 0)
      return 0;
    (void)lj_thr_retry_yield(NULL);
  }
  return 0;
}

static void test_idle_gate(void)
{
  StrictLoop loop = strict_loop_new();
  IdleReclaimCtx ctx;
  pthread_t reclaimer;

  /* Fail before arming the blocking hook if this fresh universe is not an
  ** eligible real IDLE-reclaim owner. This also proves the same production
  ** enter/leave pair reopens the gate before the raced pass below. */
  assert(lj_gc2_test_idle_reclaim_enter(loop.g));
  assert(gc2_smr_reclaiming_acq(loop.g) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(gc2_jit_phase_gate_acq(loop.g) == 0);
  lj_gc2_test_idle_reclaim_leave(loop.g);
  assert(gc2_smr_reclaiming_acq(loop.g) == LJ_GC2_SMR_OPEN);
  assert(gc2_jit_phase_gate_acq(loop.g) != 0);

  ctx.g = loop.g;
  ctx.epoch = lj_gc2_retire_epoch(loop.g) + 1u;
  ctx.reclaimed = ~(uint32_t)0;
  ctx.done = 0;

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  gc2_jit_sweep_displaced_rel(loop.g, 0);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&reclaimer, NULL, idle_reclaim_main, &ctx) == 0);
  if (!wait_for_idle_pause(&ctx)) {
    lj_gc2_test_idle_reclaim_release();
    assert(!"IDLE reclaimer missed its post-quiescence pause");
  }

  assert(gc2_phase_acq(loop.g) == LJ_GC2_IDLE);
  assert(gc2_smr_reclaiming_acq(loop.g) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(gc2_jit_phase_gate_acq(loop.g) == 0);
  assert(!lj_gc2_jit_entry_open(loop.g));
  assert(!lj_tg_any_jit_active(loop.g));
  call_cached_loop(&loop, 210);
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_trace_test_root_entry_startins_calls() != 0);
  assert(gc2_jit_sweep_displaced_acq(loop.g) == 1);
  assert(lj_tg_load_jit_base(loop.tg) == NULL);
  assert(!lj_tg_any_jit_active(loop.g));
  assert(gc2_jit_phase_gate_acq(loop.g) == 0);
  expect_strict_trace(&loop);

  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(reclaimer, NULL) == 0);
  assert(la_load32_acq(&ctx.done) == 1);
  assert(gc2_smr_reclaiming_acq(loop.g) == LJ_GC2_SMR_OPEN);
  assert(gc2_phase_acq(loop.g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(loop.g) != 0);
  assert(lj_gc2_jit_entry_open(loop.g));
  lua_close(loop.L);
}

static void *gate_close_main(void *arg)
{
  GateCloseCtx *ctx = (GateCloseCtx *)arg;
  uint64_t started;
  uint32_t expected_phase = ctx->kind == GATE_CLOSE_MARK ?
	LJ_GC2_MARK : LJ_GC2_SWEEP;

  if (!wait_for_postadmission()) {
    la_store32_rel(&ctx->failed, 1);
    la_store32_rel(&ctx->done, 1);
    return NULL;
  }
  la_store32_rel(&ctx->saw_pause, 1);
  if (lj_tg_load_jit_base(ctx->tg) != NULL)
    la_store32_rel(&ctx->saw_jit_base_before, 1);

  started = lj_thr_now_ns();
  if (ctx->kind == GATE_CLOSE_MARK)
    lj_gc2_jit_mark_request_exit(ctx->g);
  else
    lj_gc2_jit_sweep_request_exit(ctx->g);
  ctx->elapsed_ns = lj_thr_now_ns() - started;

  if (lj_tg_load_jit_base(ctx->tg) != NULL)
    la_store32_rel(&ctx->saw_jit_base_after, 1);
  if (gc2_jit_phase_gate_acq(ctx->g) == 0)
    la_store32_rel(&ctx->gate_closed, 1);
  if (gc2_phase_acq(ctx->g) == expected_phase)
    la_store32_rel(&ctx->phase_stable, 1);
  if (gc2_hs_epoch_acq(ctx->g) == ctx->epoch &&
      lj_tg_hs_epoch_ack_acq(ctx->tg) == ctx->ack &&
      gc2_hs_pending_acq(ctx->g) == 0 &&
      gc2_hs_leader_acq(ctx->g) == 0 &&
      lj_tg_reqmask_acq(ctx->tg) == 0 &&
      lj_tg_poll_acq(ctx->tg) == 0 &&
      lj_tg_profile_request_acq(ctx->tg) == 0)
    la_store32_rel(&ctx->request_words_clean, 1);
  if (ctx->kind == GATE_CLOSE_SWEEP &&
      gc2_jit_sweep_displaced_acq(ctx->g) == 1)
    la_store32_rel(&ctx->sweep_displaced, 1);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void *gate_watchdog_main(void *arg)
{
  GateWatchdogCtx *ctx = (GateWatchdogCtx *)arg;
  uint64_t started;

  if (!wait_for_postadmission()) {
    la_store32_rel(&ctx->timed_out, 1);
    lj_trace_test_root_entry_release();
    la_store32_rel(&ctx->released, 1);
    return NULL;
  }
  la_store32_rel(&ctx->saw_pause, 1);
  started = lj_thr_now_ns();
  while (before_deadline(started)) {
    if (la_load32_acq(&ctx->closer->done) != 0)
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  if (la_load32_acq(&ctx->closer->done) == 0)
    la_store32_rel(&ctx->timed_out, 1);
  lj_trace_test_root_entry_release();
  la_store32_rel(&ctx->released, 1);
  return NULL;
}

static void expect_gate_xpoll(StrictLoop *loop, GateCloseCtx *closer,
	GateWatchdogCtx *watchdog, GateCloseKind kind, uint32_t phase)
{
  global_State *g = loop->g;
  TGState *tg = loop->tg;
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  uint32_t exits = lj_trace_test_exit_calls();
  uint32_t expected_publishes = kind == GATE_CLOSE_MARK ? 2u : 1u;
  uint32_t expected_exits = kind == GATE_CLOSE_MARK ? 2u : 1u;
  ExitNo expected_last = kind == GATE_CLOSE_MARK ? 8u : 5u;

  if (publishes != expected_publishes || cleanups != 0 ||
      exits != expected_exits ||
      lj_trace_test_first_exit_parent() != 1 ||
      lj_trace_test_first_exitno() != 5 ||
      lj_trace_test_last_exit_parent() != 1 ||
      lj_trace_test_last_exitno() != expected_last) {
    fprintf(stderr,
      "ARM64 phase-gate mismatch: want_phase=%u phase=%u gate=%u "
      "pub=%u clean=%u exits=%u first=%u/%u last=%u/%u startins=%u "
      "epoch=%llu/%llu ack=%llu/%llu pending=%u leader=%u "
      "req=%u poll=%u profile=%u displaced=%u\n",
      phase, gc2_phase_acq(g), gc2_jit_phase_gate_acq(g),
      publishes, cleanups, exits,
      (unsigned)lj_trace_test_first_exit_parent(),
      (unsigned)lj_trace_test_first_exitno(),
      (unsigned)lj_trace_test_last_exit_parent(),
      (unsigned)lj_trace_test_last_exitno(),
      lj_trace_test_root_entry_startins_calls(),
      (unsigned long long)closer->epoch,
      (unsigned long long)gc2_hs_epoch_acq(g),
      (unsigned long long)closer->ack,
      (unsigned long long)lj_tg_hs_epoch_ack_acq(tg),
      gc2_hs_pending_acq(g), gc2_hs_leader_acq(g),
      lj_tg_reqmask_acq(tg), lj_tg_poll_acq(tg),
      lj_tg_profile_request_acq(tg), gc2_jit_sweep_displaced_acq(g));
  }

  assert(la_load32_acq(&closer->failed) == 0);
  assert(la_load32_acq(&closer->done) == 1);
  assert(la_load32_acq(&closer->saw_pause) == 1);
  assert(la_load32_acq(&closer->saw_jit_base_before) == 1);
  assert(la_load32_acq(&closer->saw_jit_base_after) == 1);
  assert(la_load32_acq(&closer->gate_closed) == 1);
  assert(la_load32_acq(&closer->phase_stable) == 1);
  assert(la_load32_acq(&closer->request_words_clean) == 1);
  if (kind == GATE_CLOSE_SWEEP)
    assert(la_load32_acq(&closer->sweep_displaced) == 1);
  assert(closer->elapsed_ns < CLOSE_FAST_NS);
  assert(la_load32_acq(&watchdog->saw_pause) == 1);
  assert(la_load32_acq(&watchdog->timed_out) == 0);
  assert(la_load32_acq(&watchdog->released) == 1);

  assert(publishes == expected_publishes);
  assert(cleanups == 0);
  assert(exits == expected_exits);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == 5);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == expected_last);

  if (kind == GATE_CLOSE_MARK) {
    /* vm_exit_interp observes the closed MARK gate at snapshot 5, performs a
    ** bounded collector turn, completes this tiny fresh-state cycle, and then
    ** legally re-enters under IDLE for the ordinary snapshot-8 loop exit. */
    assert(lj_trace_test_root_entry_startins_calls() == 0);
    assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
    assert(gc2_jit_phase_gate_acq(g) != 0);
    assert(lj_gc2_jit_entry_open(g));
    assert(gc2_hs_epoch_acq(g) > closer->epoch);
    assert(lj_tg_hs_epoch_ack_acq(tg) == gc2_hs_epoch_acq(g));
  } else {
    assert(lj_trace_test_root_entry_startins_calls() != 0);
    assert(gc2_phase_acq(g) == phase);
    assert(gc2_jit_phase_gate_acq(g) == 0);
    assert(!lj_gc2_jit_entry_open(g));
    assert(gc2_hs_epoch_acq(g) == closer->epoch);
    assert(lj_tg_hs_epoch_ack_acq(tg) == closer->ack);
  }
  assert(gc2_hs_pending_acq(g) == 0);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(!lj_tg_any_jit_active(g));
  expect_strict_trace(loop);
}

static void run_gate_close(StrictLoop *loop, GateCloseKind kind,
	uint32_t phase)
{
  GateCloseCtx closer = {
    loop->g, loop->tg, kind,
    gc2_hs_epoch_acq(loop->g), lj_tg_hs_epoch_ack_acq(loop->tg),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };
  GateWatchdogCtx watchdog = { &closer, 0, 0, 0 };
  pthread_t closer_thread, watchdog_thread;

  assert(closer.ack == closer.epoch);
  assert(gc2_hs_pending_acq(loop->g) == 0);
  assert(gc2_hs_leader_acq(loop->g) == 0);
  assert(lj_tg_reqmask_acq(loop->tg) == 0);
  assert(lj_tg_poll_acq(loop->tg) == 0);
  assert(lj_tg_profile_request_acq(loop->tg) == 0);
  assert(gc2_phase_acq(loop->g) == phase);
  assert(gc2_jit_phase_gate_acq(loop->g) != 0);
  assert(lj_gc2_jit_entry_open(loop->g));

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  gc2_jit_sweep_displaced_rel(loop->g, 0);
  lj_trace_test_root_entry_pause(
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  assert(pthread_create(&closer_thread, NULL, gate_close_main, &closer) == 0);
  assert(pthread_create(&watchdog_thread, NULL, gate_watchdog_main,
		&watchdog) == 0);
  call_cached_loop(loop, 210);
  assert(pthread_join(closer_thread, NULL) == 0);
  assert(pthread_join(watchdog_thread, NULL) == 0);
  assert(lj_trace_test_root_entry_paused() == 0);
  expect_gate_xpoll(loop, &closer, &watchdog, kind, phase);
}

static void test_mark_gate(void)
{
  StrictLoop loop = strict_loop_new();
  global_State *g = loop.g;
  TGState *tg = loop.tg;
  uint32_t i;

  lj_gc_threshold_store(g, LJ_MAX_MEM);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_jit_mark_resume_acq(g) == gc2_cycle_acq(g));
  assert(lj_tg_mark_active_acq(tg) != 0);
  assert(lj_tg_alloc_black_acq(tg) != 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_gc2_jit_entry_open(g));

  run_gate_close(&loop, GATE_CLOSE_MARK, LJ_GC2_MARK);
  for (i = 0; i < 10000u && gc2_phase_acq(g) != LJ_GC2_IDLE; i++) {
    lj_gc2_preserve_abort_to_idle(g);
    if (gc2_phase_acq(g) != LJ_GC2_IDLE)
      (void)lj_thr_retry_yield(NULL);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_jit_mark_resume_acq(g) == 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_gc2_jit_entry_open(g));
  expect_strict_trace(&loop);
  lua_close(loop.L);
}

static void seed_sweep_garbage(lua_State *L)
{
  uint32_t i, j;
  for (i = 0; i < 20000u; i++) {
    lua_createtable(L, 8, 0);
    for (j = 1; j <= 8; j++) {
      lua_pushinteger(L, (lua_Integer)(i + j));
      lua_rawseti(L, -2, (int)j);
    }
    lua_pop(L, 1);
  }
}

static void drive_to_open_sweep(lua_State *L, global_State *g)
{
  uint32_t i;
  lua_gc(L, LUA_GCRESTART, 0);
  for (i = 0; i < 2000000u; i++) {
    (void)lua_gc(L, LUA_GCSTEP, 1);
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	gc2_sweep_bridge_ready_acq(g) != 0 &&
	gc2_jit_phase_gate_acq(g) != 0)
      return;
  }
  fprintf(stderr, "ARM64 phase-gate fixture never reached open SWEEP: %u\n",
	  gc2_phase_acq(g));
  assert(!"GC2 did not reach an open SWEEP window");
}

static void drive_to_idle(lua_State *L, global_State *g)
{
  uint32_t i;
  lua_gc(L, LUA_GCRESTART, 0);
  for (i = 0; i < 2000000u && gc2_phase_acq(g) != LJ_GC2_IDLE; i++)
    (void)lua_gc(L, LUA_GCSTEP, 1);
  if (gc2_phase_acq(g) != LJ_GC2_IDLE) {
    fprintf(stderr, "ARM64 phase-gate cleanup remained in phase %u\n",
	    gc2_phase_acq(g));
    assert(!"GC2 phase cleanup did not reach IDLE");
  }
}

static void test_sweep_gate(void)
{
  StrictLoop loop = strict_loop_new();
  global_State *g = loop.g;

  seed_sweep_garbage(loop.L);
  drive_to_open_sweep(loop.L, g);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(gc2_sweep_bridge_ready_acq(g) != 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_gc2_jit_entry_open(g));
  lua_gc(loop.L, LUA_GCSTOP, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(gc2_sweep_bridge_ready_acq(g) != 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);

  run_gate_close(&loop, GATE_CLOSE_SWEEP, LJ_GC2_SWEEP);
  assert(gc2_jit_sweep_displaced_acq(g) == 1);
  drive_to_idle(loop.L, g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  /* bridge_ready is phase-qualified. Normal SWEEP completion may retain the
  ** previous generation's certificate in IDLE; the next SWEEP transition
  ** clears it before publishing its own root certificate. */
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_gc2_jit_entry_open(g));
  expect_strict_trace(&loop);
  lua_close(loop.L);
}

int main(void)
{
  test_idle_gate();
  test_mark_gate();
  test_sweep_gate();
  puts("t-arm64-jit-phase-gates OK: IDLE veto and MARK/SWEEP XPOLL exits");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-phase-gates SKIP");
  return 0;
}

#endif
