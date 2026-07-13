/*
** x64 native trace execution during bounded GC2 MARK leases.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if LJ_TARGET_X64
static uint32_t mark_probe_hits;

typedef struct AsyncMarkCloseCtx {
  global_State *g;
  uint32_t saw_active;
  uint32_t saw_closed;
  uint64_t elapsed_ns;
} AsyncMarkCloseCtx;

typedef struct BlockingNativeCtx {
  TGState *tg;
  TValue *base;
  uint32_t ready;
} BlockingNativeCtx;

static void *blocking_native_main(void *arg)
{
  BlockingNativeCtx *ctx = (BlockingNativeCtx *)arg;
  lj_tg_store_jit_base(ctx->tg, ctx->base);
  la_store32_rel(&ctx->ready, 1);
  (void)lj_thr_sleep_ns(NULL, 100000000);
  lj_tg_store_jit_base(ctx->tg, NULL);
  return NULL;
}

static void *async_mark_close_main(void *arg)
{
  AsyncMarkCloseCtx *ctx = (AsyncMarkCloseCtx *)arg;
  uint64_t start;
  (void)lj_thr_sleep_ns(NULL, 1000000);
  ctx->saw_active = (uint32_t)lj_tg_any_jit_active(ctx->g);
  start = lj_thr_now_ns();
  lj_gc2_jit_mark_request_exit(ctx->g);
  ctx->elapsed_ns = lj_thr_now_ns() - start;
  ctx->saw_closed = gc2_jit_phase_gate_acq(ctx->g) == 0;
  return NULL;
}

static int has_xpoll_trace(global_State *g)
{
  jit_State *J = G2J(g);
  TraceNo traceno;
  for (traceno = 1; traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref(J, traceno);
    if (T && trace_traceno_acq(T) == traceno) {
      IRIns *ir = trace_ir_acq(T);
      IRRef i, nins = trace_nins_acq(T);
      for (i = REF_FIRST; i < nins; i++)
	if (ir[i].o == IR_XPOLL)
	  return 1;
    }
  }
  return 0;
}

static void publish_ptr(lua_State *L, const char *name, void *ptr)
{
  lua_pushlightuserdata(L, ptr);
  lua_setglobal(L, name);
}

static void begin_open_mark(global_State *g)
{
  TGState *tg = G2TG(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  /* The C-owned generation is published only after the synchronous activation
  ** handshake and exact request-token consumption. */
  assert(gc2_hs_pending_acq(g) == 0);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_cycle_leader_acq(g) == 0);
  assert(gc2_jit_mark_resume_acq(g) == gc2_cycle_acq(g));
  assert(lj_tg_mark_active_acq(tg) != 0);
  assert(lj_tg_alloc_black_acq(tg) != 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_gc2_jit_entry_open(g));
}

static void drive_to_idle(lua_State *L, global_State *g)
{
  uint32_t i;
  for (i = 0; i < 2000000u && gc2_phase_acq(g) != LJ_GC2_IDLE; i++)
    (void)lua_gc(L, LUA_GCSTEP, 1);
  if (gc2_phase_acq(g) != LJ_GC2_IDLE) {
    fprintf(stderr, "cooperative MARK cycle remained in phase %u\n",
	    gc2_phase_acq(g));
    assert(0);
  }
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(gc2_jit_mark_resume_acq(g) == 0);
}

static void test_stalled_recorder_snapshot_retry(lua_State *L,
						 global_State *g)
{
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  uint32_t cycle, owner, i;

  begin_open_mark(g);
  cycle = gc2_cycle_acq(g);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);

  /* Model a recorder descheduled while holding its exact token. The root
  ** scanner cannot certify mutable J->cur and requests an asynchronous abort.
  ** Keeping the token held after that request models a recorder which has not
  ** yet returned to its state machine to unwind the private geometry. */
  owner = lj_tg_tid_acq(tg) + 1u;
  if (owner == 0 || owner == lj_tg_tid_acq(tg))
    owner = lj_tg_tid_acq(tg) ^ 0x80000000u;
  if (owner == 0 || owner == lj_tg_tid_acq(tg))
    owner = 1u;
  assert(owner != 0 && owner != lj_tg_tid_acq(tg));
  jit_token_rel(g, owner);
  lj_trace_state_store(J, LJ_TRACE_RECORD);

  for (i = 0; i < 2000000u && !lj_trace_state_aborted(
	 lj_trace_state_load(J)); i++)
    (void)lj_gc2_mark_complete(g, L, 1, LJ_GC2_WORKER_DRAIN_BATCH);
  assert(lj_trace_state_aborted(lj_trace_state_load(J)));
  assert(jit_token_acq(g) == owner);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_acq(g) == cycle);
  /* An incomplete recorder scan must invalidate the in-progress state-2 snapshot.
  ** Certifying state 1 here lets the next round skip the recorder forever. */
  assert(gc2_mark_root_scanned_acq(g) == 0);

  for (i = 0; i < 32u; i++) {
    assert(!lj_gc2_mark_complete(g, L, 4,
				 LJ_GC2_WORKER_DRAIN_BATCH));
    assert(gc2_phase_acq(g) == LJ_GC2_MARK);
    assert(gc2_cycle_acq(g) == cycle);
    assert(gc2_mark_root_scanned_acq(g) == 0);
    assert(jit_token_acq(g) == owner);
  }

  /* Model the recorder observing the asynchronous abort and releasing its
  ** token. A later closed snapshot can now certify the roots and finish. */
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  jit_token_rel(g, 0);
  drive_to_idle(L, g);

  /* The retry belongs only to the generation that observed the stalled
  ** recorder. A later activation must publish a fresh generation/token rather
  ** than inheriting either the invalid snapshot or its retry state. */
  begin_open_mark(g);
  assert(gc2_cycle_acq(g) != cycle);
  assert(gc2_jit_mark_resume_acq(g) == gc2_cycle_acq(g));
  assert(gc2_mark_root_scanned_acq(g) == 0);
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static void test_same_owner_recorder_snapshot_retry(lua_State *L,
					     global_State *g)
{
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace savedcur;
  lua_State *savedowner;
  uint32_t cycle, tid, i;

  begin_open_mark(g);
  cycle = gc2_cycle_acq(g);
  tid = lj_tg_tid_acq(tg);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
  assert(tid != 0);

  /* Model an allocation/GC check nested inside the recorder which owns the
  ** token on this same TG. An old scanner treated token ownership as a frozen
  ** J->cur snapshot and could certify state 1, after which recording appended
  ** further NOBARRIER KGC/snapshot roots. Keep geometry deliberately empty so
  ** that old behavior would incorrectly reach WEAK rather than fail validation.
  */
  savedcur = J->cur;
  savedowner = jit_owner_l_acq(J);
  memset(&J->cur, 0, sizeof(J->cur));
  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
  lj_trace_state_store(J, LJ_TRACE_RECORD);
  assert(lj_jit_token_held(J));

  for (i = 0; i < 8u; i++) {
    assert(!lj_gc2_mark_complete(g, L, 4,
				 LJ_GC2_WORKER_DRAIN_BATCH));
    assert(lj_trace_state_aborted(lj_trace_state_load(J)));
    assert(gc2_phase_acq(g) == LJ_GC2_MARK);
    assert(gc2_cycle_acq(g) == cycle);
    assert(gc2_mark_root_scanned_acq(g) == 0);
    assert(jit_token_acq(g) == tid);
  }

  /* Model the owner observing the asynchronous abort at its recorder state
  ** boundary. Only the subsequent IDLE snapshot may become persistent. */
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  assert(jit_token_acq(g) == 0);
  J->cur = savedcur;
  jit_owner_l_rel(J, savedowner);
  drive_to_idle(L, g);
}

static void test_same_owner_fullgc_defers(lua_State *L, global_State *g)
{
  jit_State *J = G2J(g);
  GCtrace savedcur;
  lua_State *savedowner;
  uint64_t cycles0;

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
  cycles0 = gc2_cycle_starts_acq(g);
  savedcur = J->cur;
  savedowner = jit_owner_l_acq(J);
  memset(&J->cur, 0, sizeof(J->cur));
  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
  lj_trace_state_store(J, LJ_TRACE_RECORD);

  /* A nested full collection cannot wait for this exact recorder to unwind,
  ** because the unwind is sequenced after the API call returns. It must only
  ** publish the abort and defer without starting a cycle. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lj_trace_state_aborted(lj_trace_state_load(J)));
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_starts_acq(g) == cycles0);
  assert(lj_jit_token_held_l(L, J));

  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  assert(jit_token_acq(g) == 0);
  J->cur = savedcur;
  jit_owner_l_rel(J, savedowner);
}

static void run_async_close(lua_State *L, global_State *g,
			    const char *func, lua_Integer arg,
			    int nresults, AsyncMarkCloseCtx *ctx)
{
  pthread_t closer;
  ctx->g = g;
  ctx->saw_active = 0;
  ctx->saw_closed = 0;
  ctx->elapsed_ns = ~(uint64_t)0;
  assert(pthread_create(&closer, NULL, async_mark_close_main, ctx) == 0);
  lua_getglobal(L, func);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, arg);
  ljt_lua_pcall(L, 1, nresults, func);
  assert(pthread_join(closer, NULL) == 0);
  if (!ctx->saw_active)
    fprintf(stderr, "%s close missed native execution\n", func);
  assert(ctx->saw_active != 0);
  assert(ctx->saw_closed != 0);
  assert(ctx->elapsed_ns < 50000000u);
}

static void test_automatic_mark_balance(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t alloc0, alloc1, cycles0, cycles1;
  GCSize total0, total1;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(g != NULL);
  /* Use an isolated VM so prior synthetic phase/cadence manipulation cannot
  ** subsidize this balance check. The top-level hot loop disables sinking, so
  ** all 200000 non-empty tables are real births and only the final one lives. */
  ljt_lua_dostring(L,
    "collectgarbage('restart')\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n");
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  total0 = lj_gc_total_load(g);
  alloc0 = gc2_alloc_total_bytes_acq(g);
  cycles0 = gc2_cycle_starts_acq(g);

  ljt_lua_dostring(L,
    "local x\n"
    "local i = 1\n"
    "while i <= 200000 do x = { i, i + 1, i + 2 }; i = i + 1 end\n"
    "assert(x[1] == 200000 and require('jit.util').traceinfo(1))\n"
    "__gc2_mark_auto_result = x\n");
  lua_getglobal(L, "__gc2_mark_auto_result");
  assert(lua_istable(L, -1));
  lua_rawgeti(L, -1, 1);
  assert(lua_tointeger(L, -1) == 200000);
  lua_pop(L, 1);

  total1 = lj_gc_total_load(g);
  alloc1 = gc2_alloc_total_bytes_acq(g);
  cycles1 = gc2_cycle_starts_acq(g);
  /* An unconditional interpreter-side lease yield used to finish this loop in
  ** active SWEEP after only ~2 cycles with >20MiB retained. Automatic checks
  ** with no published native frame must instead pay bounded collector debt. */
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(alloc1 - alloc0 >= 16u * 1024u * 1024u);
  assert(cycles1 - cycles0 >= 8u);
  assert(total1 <= total0 + 4u * 1024u * 1024u);
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "__gc2_mark_auto_result");
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  uint32_t worker_actions = 0;
  AsyncMarkCloseCtx closectx;
#if defined(LJ_GC2_TEST_HELPERS)
  uint64_t hard_check0;
#endif

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(g != NULL && tg != NULL);
  assert(lj_gc2_workers_set_l(L, 1, &worker_actions) == 1);
  assert(gc2_n_workers_acq(g) == 1);

  publish_ptr(L, "__gc2_mark_phase_ptr", &g->gc2.phase);
  publish_ptr(L, "__gc2_mark_gate_ptr", &g->gc2.jit_phase_gate);
  publish_ptr(L, "__gc2_mark_vmstate_ptr", &tg->vmstate);
  publish_ptr(L, "__gc2_mark_hits_ptr", &mark_probe_hits);
  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local ffi = require('ffi')\n"
    "local util = require('jit.util')\n"
    "local bit = require('bit')\n"
    "local phase = ffi.cast('volatile uint32_t *', __gc2_mark_phase_ptr)\n"
    "local gate = ffi.cast('volatile uint32_t *', __gc2_mark_gate_ptr)\n"
    "local vmstate = ffi.cast('volatile int32_t *', __gc2_mark_vmstate_ptr)\n"
    "local hits = ffi.cast('volatile uint32_t *', __gc2_mark_hits_ptr)\n"
    "function __gc2_mark_alloc(out, n)\n"
    "  for i = 1, n do\n"
    "    local native = bit.rshift(bit.bnot(vmstate[0]), 31)\n"
    "    local x = bit.bxor(phase[0], 1)\n"
    "    local ismark = bit.rshift(bit.bnot(bit.bor(x, -x)), 31)\n"
    "    hits[0] = hits[0] + bit.band(native, ismark, gate[0])\n"
    "    out[i] = { i, i + 1 }\n"
    "  end\n"
    "  return out\n"
    "end\n"
    "function __gc2_mark_register(n)\n"
    "  local only = { magic = 0x51a7 }\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x + i end\n"
    "  return only, x\n"
    "end\n"
    "assert(type(__gc2_mark_alloc({}, 400)) == 'table')\n"
    "for _ = 1, 20 do\n"
    "  local o, x = __gc2_mark_register(400)\n"
    "  assert(o.magic == 0x51a7 and x == 80200)\n"
    "end\n"
    "local found = false\n"
    "for i = 1, 100 do if util.traceinfo(i) then found = true end end\n"
    "assert(found, 'MARK cooperative fixtures did not trace')\n"
    "__gc2_mark_keep = {}\n"
    "for i = 1, 20000 do __gc2_mark_keep[i] = { i, 'root' .. i } end\n");
  assert(has_xpoll_trace(g));
  assert(lj_gc2_workers_set_l(L, 0, &worker_actions) == 1);
  assert(gc2_n_workers_acq(g) == 0);

  /* Zero-worker traced allocation executes in MARK. Its common accounting
  ** and hard-cadence paths remain active while automatic interpreter checks
  ** pay enough collector debt to complete the cycle. */
  la_store32_rel(&mark_probe_hits, 0);
  lua_newtable(L);  /* Avoid a pre-trace allocation after MARK begins. */
  begin_open_mark(g);
  /* Isolate native MARK admission from the interpreter debt driver in every
  ** build. The fresh-state balance fixture below covers automatic pacing. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
#if defined(LJ_GC2_TEST_HELPERS)
  la_store64_rel(&g->gc2.alloc_since_trigger, LJ_GC2_ACCT_FLUSH * 2u);
  la_store64_rel(&g->gc2.hard_bytes, LJ_GC2_ACCT_FLUSH);
  lj_gc2_hard_check_store(g, LJ_GC2_ACCT_FLUSH * 2u);
  la_store64_rel(&tg->local_total, LJ_GC2_ACCT_FLUSH - 1u);
  hard_check0 = lj_gc2_hard_check_load(g);
#endif
  lua_getglobal(L, "__gc2_mark_alloc");
  lua_insert(L, -2);
  lua_pushinteger(L, 80000);
  ljt_lua_pcall(L, 2, 1, "cooperative MARK allocation trace");
  assert(lua_istable(L, -1));
  assert(la_load32_acq(&mark_probe_hits) != 0);
#if defined(LJ_GC2_TEST_HELPERS)
  assert(lj_gc2_hard_check_load(g) > hard_check0);
#endif
  lua_pop(L, 1);
  drive_to_idle(L, g);

  /* A recorder-token miss cannot become a persistent root-snapshot
  ** certificate while the aborted recorder remains stalled on its token. */
  test_same_owner_recorder_snapshot_retry(L, g);
  test_stalled_recorder_snapshot_retry(L, g);
  test_same_owner_fullgc_defers(L, g);

  /* Close a long-running XPOLL trace asynchronously. The table exists only in
  ** the native frame/snapshot when the close lands; ordinary exit restoration
  ** must make it visible to the subsequent closed root snapshot. */
  ljt_lua_dostring(L,
    "for _ = 1, 4 do\n"
    "  local o, x = __gc2_mark_register(2000)\n"
    "  assert(o.magic == 0x51a7 and x == 2001000)\n"
    "end\n");
  if (gc2_phase_acq(g) == LJ_GC2_IDLE) {
    begin_open_mark(g);
  } else {
    assert(gc2_phase_acq(g) == LJ_GC2_MARK);
    assert(gc2_jit_phase_gate_acq(g) != 0);
    assert(gc2_jit_mark_resume_acq(g) == gc2_cycle_acq(g));
  }
  run_async_close(L, g, "__gc2_mark_register", 50000000, 2, &closectx);
  assert(lua_istable(L, -2));
  assert(lua_isnumber(L, -1));
  lua_getfield(L, -2, "magic");
  assert(lua_tointeger(L, -1) == 0x51a7);
  lua_pop(L, 1);
  drive_to_idle(L, g);
  lua_getfield(L, -2, "magic");
  assert(lua_tointeger(L, -1) == 0x51a7);
  lua_pop(L, 3);

  /* Model the exact jit_base publication retained across a blocking traced FFI
  ** call. MARK close must defer rather than wait for the foreign call's native
  ** return to clear that publication. */
  {
    BlockingNativeCtx blockctx;
    pthread_t blocker;
    uint64_t start, elapsed, progress0, progress1;
    begin_open_mark(g);
    assert(lj_gc2_workers_set_l(L, 1, &worker_actions) == 1);
    assert(gc2_n_workers_acq(g) == 1);
    blockctx.tg = tg;
    blockctx.base = L->base;
    la_store32_rlx(&blockctx.ready, 0);
    assert(pthread_create(&blocker, NULL, blocking_native_main, &blockctx) == 0);
    while (la_load32_acq(&blockctx.ready) == 0)
      la_cpu_pause();
    assert(lj_tg_any_jit_active(g));
    progress0 = gc2_worker_async_progress_acq(g);
    start = lj_thr_now_ns();
    assert(!lj_gc2_mark_complete(g, L, 1, LJ_GC2_WORKER_DRAIN_BATCH));
    elapsed = lj_thr_now_ns() - start;
    assert(elapsed < 50000000u);
    assert(gc2_jit_phase_gate_acq(g) == 0);
    (void)lj_thr_sleep_ns(NULL, 20000000);
    progress1 = gc2_worker_async_progress_acq(g);
    /* A retained Boolean close intent used to manufacture one fake progress
    ** unit per worker loop while FFI was blocked. No real mark work is legal
    ** until jit_base clears, so only a small pre-observation scheduling skew is
    ** tolerated here. */
    assert(progress1 - progress0 < 100u);
    assert(pthread_join(blocker, NULL) == 0);
    assert(!lj_tg_any_jit_active(g));
    assert(lj_gc2_workers_set_l(L, 0, &worker_actions) == 1);
    assert(gc2_n_workers_acq(g) == 0);
    drive_to_idle(L, g);
  }

  /* A worker attached after activation adopts MARK barriers before becoming
  ** live. Its background drain may alternate bounded native/collector turns;
  ** deactivation and preserve-abort remain exact. */
  begin_open_mark(g);
  assert(lj_gc2_workers_set_l(L, 1, &worker_actions) == 1);
  assert(gc2_n_workers_acq(g) == 1);
  {
    uint32_t i;
    for (i = 0; i < 1000000u; i++) {
      lj_gc2_jit_mark_request_exit(g);
      if (gc2_jit_phase_gate_acq(g) == 0)
	break;
      la_cpu_pause();
    }
    assert(gc2_jit_phase_gate_acq(g) == 0);
    for (i = 0; i < 10000000u && gc2_jit_phase_gate_acq(g) == 0; i++)
      la_cpu_pause();
    assert(gc2_jit_phase_gate_acq(g) != 0);
    assert(gc2_jit_mark_yield_until_ns_acq(g) != 0);
  }
  assert(lj_gc2_workers_set_l(L, 0, &worker_actions) == 1);
  assert(gc2_n_workers_acq(g) == 0);
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(g) != 0);

  /* A successful root/fixpoint close publishes WEAK while native admission is
  ** still closed. WEAK must never consume the MARK resume generation. */
  {
    uint32_t i;
    begin_open_mark(g);
    for (i = 0; i < 2000000u && gc2_phase_acq(g) == LJ_GC2_MARK; i++)
      (void)lua_gc(L, LUA_GCSTEP, 1);
    assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
    assert(gc2_jit_phase_gate_acq(g) == 0);
    assert(gc2_jit_mark_resume_acq(g) == 0);
    assert(!lj_gc2_jit_entry_open(g));
    drive_to_idle(L, g);
  }

  /* Repeated activation/abort generations must not leak the MARK resume token
  ** into IDLE or allow WEAK-style closed phases to reopen native entry. */
  {
    uint32_t i;
    for (i = 0; i < 4; i++) {
      begin_open_mark(g);
      lj_gc2_preserve_abort_to_idle(g);
      assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
      assert(gc2_jit_phase_gate_acq(g) != 0);
      assert(gc2_jit_mark_resume_acq(g) == 0);
    }
  }

#if defined(LJ_GC2_TEST_HELPERS)
  /* Exercise the common allocation checkpoint at its exact native-publication
  ** boundary. Pin the fresh lease deadline so bounded assist cannot consume
  ** the turn before the checkpoint publishes its asynchronous close. */
  begin_open_mark(g);
  gc2_jit_mark_yield_until_ns_rel(g, ~(uint64_t)0);
  la_store64_rel(&g->gc2.alloc_since_trigger, LJ_GC2_ACCT_FLUSH * 2u);
  la_store64_rel(&g->gc2.hard_bytes, LJ_GC2_ACCT_FLUSH);
  lj_gc2_hard_check_store(g, LJ_GC2_ACCT_FLUSH * 2u);
  la_store64_rel(&tg->local_total, LJ_GC2_ACCT_FLUSH);
  lj_gc2_test_jit_mark_checkpoint_reset();
  lj_tg_store_jit_base(tg, L->base);
  (void)lj_gc2_flush_alloc_checkpoint(g, tg);
  lj_tg_store_jit_base(tg, NULL);
  assert(lj_gc2_test_jit_mark_checkpoint_closes() != 0);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  drive_to_idle(L, g);
#endif

  /* Interpreter-side automatic checks must continue paying collector debt.
  ** A pre-dispatch handoff leaves the threshold exactly due, and its bounded
  ** allowance falls back to the full automatic batch if no trace enters. */
  test_automatic_mark_balance();

  lua_pushnil(L); lua_setglobal(L, "__gc2_mark_keep");
  lua_pushnil(L); lua_setglobal(L, "__gc2_mark_alloc");
  lua_pushnil(L); lua_setglobal(L, "__gc2_mark_register");
  lua_pushnil(L); lua_setglobal(L, "__gc2_mark_phase_ptr");
  lua_pushnil(L); lua_setglobal(L, "__gc2_mark_gate_ptr");
  lua_pushnil(L); lua_setglobal(L, "__gc2_mark_vmstate_ptr");
  lua_pushnil(L); lua_setglobal(L, "__gc2_mark_hits_ptr");
  lua_close(L);
  puts("t-gc2-jit-mark-coop OK: x64 traces ran during bounded MARK leases");
  return 0;
}
#else
int main(void)
{
  puts("t-gc2-jit-mark-coop SKIP: cooperative entry is x64-only");
  return 0;
}
#endif
