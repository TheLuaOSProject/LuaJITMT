/*
** Focused regression test for the M6 JIT recorder token.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_target.h"
#include "lj_vmevent.h"

#include "lib/lua_fixture_helpers.h"
#include "lib/tg_stopreq_fixture_helpers.h"

typedef struct TokenReleaseCtx {
  global_State *g;
  uint32_t owner;
  uint32_t released;
} TokenReleaseCtx;

typedef struct TokenStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t owner;
  uint32_t saw_native;
  uint32_t signaled;
  uint32_t released;
} TokenStopReqCtx;

static uint32_t foreign_token_owner(lua_State *L)
{
  uint32_t self = lj_tg_tid_acq(L2TG(L));
  return self == 0x7fffffffu ? 0x7ffffffeu : 0x7fffffffu;
}

static uint32_t vmevent_calls;
static lua_State *vmevent_expected_L;

static int count_vmevent(lua_State *L)
{
  assert(L == vmevent_expected_L);
  (void)la_add32_acqrel(&vmevent_calls, 1);
  return 0;
}

static int force_vmevent_prepare_stack_growth(lua_State *L)
{
  ptrdiff_t entrytop = savestack(L, L->top);
  ptrdiff_t eventtop;
  ptrdiff_t argbase;
  MSize oldsize = L->stacksize;

  /* Leave exactly the reserve that lj_vmevent_prepare() requests. Its <= test
  ** must grow this fresh coroutine's stack before acquiring the TRACE handler.
  */
  while ((mref(L->maxstack, char) - (char *)L->top) >
	 (ptrdiff_t)LUA_MINSTACK * (ptrdiff_t)sizeof(TValue))
    setnilV(L->top++);
  eventtop = savestack(L, L->top);
  argbase = lj_vmevent_prepare(L, LJ_VMEVENT_TRACE);
  if (argbase) {
    lua_pushliteral(L, "flush");
    lj_vmevent_call(L, argbase, eventtop);
  }

  L->top = restorestack(L, entrytop);
  lua_pushboolean(L, L->stacksize > oldsize);
  return 1;
}

static void expect_vmevent_owner_and_jit_pointer(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  lua_State *sentinel = vmthread_acq(g);
  uint32_t foreign = foreign_token_owner(L);
  uint32_t expect = 0;
  uint32_t calls;
  ptrdiff_t top;

  vmevent_expected_L = L;
  lua_pushcfunction(L, count_vmevent);
  lua_setglobal(L, "lj_m6_count_vmevent");
  ljt_lua_dostring(L,
    "jit.attach(lj_m6_count_vmevent, 'trace')\n");

  /* A competing callback owner must make this event a bounded drop. Argument
  ** construction is local to L and the failed call restores the exact top. */
  calls = la_load32_acq(&vmevent_calls);
  top = savestack(L, L->top);
  assert(vmevent_owner_cas(g, &expect, foreign));
  lj_vmevent_send_l(L, TRACE,
    lua_pushliteral(V, "flush");
  );
  assert(savestack(L, L->top) == top);
  assert(la_load32_acq(&vmevent_calls) == calls);
  assert(vmevent_owner_acq(g) == foreign);
  vmevent_owner_rel(g, foreign);

  /* A process-wide GC/debug hook owner is an independent exclusion domain.
  ** VM events must drop without restoring a stale hookmask snapshot over it. */
  calls = la_load32_acq(&vmevent_calls);
  top = savestack(L, L->top);
  (void)hookmask_update(g, 0, HOOK_ACTIVE|HOOK_GC);
  lj_vmevent_send_l(L, TRACE,
    lua_pushliteral(V, "flush");
  );
  assert(savestack(L, L->top) == top);
  assert(la_load32_acq(&vmevent_calls) == calls);
  assert((hookmask_load(g) & (HOOK_ACTIVE|HOOK_GC)) ==
	 (HOOK_ACTIVE|HOOK_GC));
  assert(vmevent_owner_acq(g) == 0);
  (void)hookmask_update(g, HOOK_ACTIVE|HOOK_GC, 0);

  /* A non-recorder event may run, but it must not replace the actual J->L
  ** pointer owned by a foreign recorder TG with this event's initiating L. */
  calls = la_load32_acq(&vmevent_calls);
  jit_owner_l_rel(J, sentinel);
  jit_token_rel(g, foreign);
  lj_vmevent_send_l(L, TRACE,
    lua_pushliteral(V, "flush");
  );
  assert(la_load32_acq(&vmevent_calls) == calls + 1u);
  assert(jit_owner_l_acq(J) == sentinel);
  assert(jit_token_acq(g) == foreign);
  jit_token_rel(g, 0);
  jit_owner_l_rel(J, NULL);

  ljt_lua_dostring(L, "jit.attach(lj_m6_count_vmevent)\n");
  lua_pushnil(L);
  lua_setglobal(L, "lj_m6_count_vmevent");
  vmevent_expected_L = NULL;
}

static void expect_vmevent_smr_try_drop(lua_State *L)
{
  global_State *g = G(L);
  ptrdiff_t top;
  uint32_t expect = 0;
  uint8_t mask;

  vmevent_expected_L = L;
  lua_pushcfunction(L, count_vmevent);
  lua_setglobal(L, "lj_m6_count_vmevent");
  ljt_lua_dostring(L,
    "jit.attach(lj_m6_count_vmevent, 'trace')\n");
  mask = vmevmask_load_acq(g);
  assert((mask & VMEVENT_MASK(LJ_VMEVENT_TRACE)) != 0);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(gc2_smr_reclaiming_cas(g, &expect, 1));

  /* VM events are observational: a reclaimer collision is a one-shot drop,
  ** not a wait. It must not consume the cache bit or leak a reader count. */
  assert(lj_gc2_smr_read_try(g) == 0);
  top = savestack(L, L->top);
  assert(lj_vmevent_prepare(L, LJ_VMEVENT_TRACE) == 0);
  assert(savestack(L, L->top) == top);
  assert(vmevmask_load_acq(g) == mask);
  assert(gc2_smr_readers_acq(g) == 0);
  gc2_smr_reclaiming_rel(g, 0);

  assert(lj_gc2_smr_read_try(g) != 0);
  assert(gc2_smr_readers_acq(g) == 1);
  lj_gc2_smr_read_leave(g);
  assert(gc2_smr_readers_acq(g) == 0);

  ljt_lua_dostring(L, "jit.attach(lj_m6_count_vmevent)\n");
  lua_pushnil(L);
  lua_setglobal(L, "lj_m6_count_vmevent");
  vmevent_expected_L = NULL;
}

static GCtrace *first_live_trace(jit_State *J)
{
  TraceNo i;
  for (i = 1; i < trace_sizetrace_acq(J); i++) {
    GCtrace *T = traceref(J, i);
    if (T && T->traceno == i)
      return T;
  }
  return NULL;
}

static GCtrace *first_trace_without_runnable_inbound(jit_State *J)
{
  TraceNo targetno, sourceno;
  MSize sizetrace = trace_sizetrace_acq(J);
  for (targetno = 1; (MSize)targetno < sizetrace; targetno++) {
    GCtrace *target = traceref_safe(J, targetno);
    int inbound = 0;
    if (!trace_runnable_acq(target, targetno))
      continue;
    for (sourceno = 1; (MSize)sourceno < sizetrace; sourceno++) {
      GCtrace *source = traceref_safe(J, sourceno);
      if (source && source != target &&
	  trace_runnable_acq(source, sourceno) &&
	  trace_link_acq(source) == targetno) {
	inbound = 1;
	break;
      }
    }
    if (!inbound)
      return target;
  }
  return NULL;
}

static uint32_t trace_ir_op_count(GCtrace *T, IROp op)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef i, nins = trace_nins_acq(T);
  uint32_t n = 0;
  for (i = REF_FIRST; i < nins; i++)
    if (ir[i].o == op)
      n++;
  return n;
}

static uint32_t trace_ir_xpoll_remote_count(GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef i, nins = trace_nins_acq(T);
  uint32_t n = 0;
  for (i = REF_FIRST; i < nins; i++)
    if (ir[i].o == IR_XPOLL && ir[i].op1 != 0)
      n++;
  return n;
}

static void expect_loop_xpoll_shape(lua_State *L, int want_xpoll)
{
  jit_State *J = G2J(G(L));
  GCtrace *T;
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function lj_m6_xpoll_shape(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(lj_m6_xpoll_shape(80) == 3240) end\n"
    "assert(util.traceinfo(1), 'expected loop trace')\n");
  T = first_live_trace(J);
  assert(T != NULL);
  assert(trace_ir_op_count(T, IR_LOOP) > 0);
  assert(trace_ir_op_count(T, IR_XPOLL) > 0);  /* Gate-only is unconditional. */
  if (want_xpoll)
    assert(trace_ir_xpoll_remote_count(T) > 0);
  else
    assert(trace_ir_xpoll_remote_count(T) == 0);
}

static void *release_jit_token_after_delay(void *arg)
{
  TokenReleaseCtx *ctx = (TokenReleaseCtx *)arg;
  (void)lj_thr_sleep_ns(NULL, 30000000);
  assert(jit_token_acq(ctx->g) == ctx->owner);
  la_store32_rel(&ctx->released, 1);
  jit_token_rel(ctx->g, 0);
  return NULL;
}

static void *publish_stopreq_while_token_waits(void *arg)
{
  TokenStopReqCtx *ctx = (TokenStopReqCtx *)arg;
  int i;
  for (i = 0; i < 5000; i++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    (void)lj_thr_sleep_ns(NULL, 1000000);
  }
  assert(la_load32_acq(&ctx->saw_native) != 0);
  la_store32_rel(&ctx->signaled,
		 lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ));
  assert(jit_token_acq(ctx->g) == ctx->owner);
  jit_token_rel(ctx->g, 0);
  la_store32_rel(&ctx->released, 1);
  return NULL;
}

static void make_token_flush_trace(lua_State *L)
{
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function lj_m6_token_tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(lj_m6_token_tracecount, true)\n"
    "function lj_m6_token_flush_f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(lj_m6_token_flush_f(80) == 3240) end\n"
    "assert(lj_m6_token_tracecount() > 0, 'expected live trace')\n");
}

static void expect_gc_trace_free_defers_for_token(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  GCtrace *T;
  TraceNo traceno;
  TraceNo freetrace;
  uint32_t foreign = foreign_token_owner(L);
  uint64_t retire_epoch;

  make_token_flush_trace(L);
  /* This case isolates token admission. A runnable terminal inbound edge is a
  ** separate semantic root and correctly makes GC rescue its target. */
  T = first_trace_without_runnable_inbound(J);
  assert(T != NULL);
  traceno = trace_traceno_acq(T);
  assert(traceno != 0 && traceref_safe(J, traceno) == T);
  freetrace = J->freetrace;

  jit_token_rel(g, foreign);
  /* A GC claimant never races the recorder's target validate->publish window.
  ** Losing the single nonwaiting token try requests async abort and authorizes
  ** neither an epoch claim nor a root-spine splice.
  */
  assert(lj_trace_retire_gc_claim(g, T) == 0);
  assert(lj_trace_free_gc(g, T) == 0);
  assert(jit_token_acq(g) == foreign);
  assert(trace_traceno_acq(T) == traceno);
  assert(traceref_safe(J, traceno) == T);
  assert(J->freetrace == freetrace);
  assert(la_load64_acq(&T->retire_epoch) == 0);
  assert(!trace_retired_link_listed_acq(T));

  /* Once the recorder releases, the claimant takes the token once, publishes
  ** the encoded claim and intrusive list node, then authorizes root pruning.
  ** The detached-body destructor disconnects the exact public slot; a gated
  ** pass before the full epoch margin must retain the body and reservation.
  */
  jit_token_rel(g, 0);
  assert(lj_trace_retire_gc_claim(g, T) == 1);
  retire_epoch = la_load64_acq(&T->retire_epoch) - 1u;
  assert(trace_retired_link_listed_acq(T));
  assert(lj_trace_free_gc(g, T) == 1);
  assert(trace_traceno_acq(T) == 0);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  assert(lj_jit_token_try(J));
  /* The process-wide pass may reclaim unrelated entries retired by the
  ** fixture's preceding jit.flush(). Only this body's pre-margin retention is
  ** part of the assertion. */
  (void)lj_trace_reclaim_retired(g, retire_epoch + 1u);
  lj_jit_token_release(J);
  lj_gc2_test_idle_reclaim_leave(g);
  assert(jit_token_acq(g) == 0);
  assert(trace_retired_link_listed_acq(T));
  assert(trace_traceno_acq(T) == 0);
  /* Sticky MT keeps the exact retired body in the slot until SMR grace. */
  assert(traceref_safe(J, traceno) == T);
  /* J->freetrace may move for an unrelated older body reclaimed by the same
  ** process-wide pass; the exact target slot remaining T proves it was not
  ** released for reuse. */
}

static void expect_flush_waits_for_token(lua_State *L, const char *code,
					 int expect_all_flushed)
{
  TokenReleaseCtx ctx;
  pthread_t th;
  global_State *g = G(L);
  make_token_flush_trace(L);
  ctx.g = g;
  ctx.owner = foreign_token_owner(L);
  ctx.released = 0;
  jit_token_rel(g, ctx.owner);
  assert(pthread_create(&th, NULL, release_jit_token_after_delay, &ctx) == 0);
  ljt_lua_dostring(L, code);
  assert(pthread_join(th, NULL) == 0);
  assert(la_load32_acq(&ctx.released) == 1);
  assert(jit_token_acq(g) == 0);
  if (expect_all_flushed)
    ljt_lua_dostring(L, "assert(lj_m6_token_tracecount() == 0)\n");
}

static void expect_opt_start_waits_for_token(lua_State *L)
{
  TokenReleaseCtx ctx;
  pthread_t th;
  global_State *g = G(L);
  ctx.g = g;
  ctx.owner = foreign_token_owner(L);
  ctx.released = 0;
  jit_token_rel(g, ctx.owner);
  assert(pthread_create(&th, NULL, release_jit_token_after_delay, &ctx) == 0);
  ljt_lua_dostring(L, "jit.opt.start('hotloop=3', 'hotexit=4', '-sink')\n");
  assert(pthread_join(th, NULL) == 0);
  assert(la_load32_acq(&ctx.released) == 1);
  assert(jit_token_acq(g) == 0);
  assert(jit_param_acq(G2J(g), JIT_P_hotloop) == 3);
  assert(jit_param_acq(G2J(g), JIT_P_hotexit) == 4);
  assert((jit_flags_acq(G2J(g)) & JIT_F_OPT_SINK) == 0);

  ljt_lua_dostring(L,
    "local ok = pcall(jit.opt.start, 'hotloop=5', 'not_an_option')\n"
    "assert(not ok)\n");
  assert(jit_token_acq(g) == 0);
  assert(jit_param_acq(G2J(g), JIT_P_hotloop) == 5);
}

static void expect_token_wait_stopreq(lua_State *L, const char *code)
{
  TokenStopReqCtx ctx;
  pthread_t th;
  global_State *g = G(L);
  int status;

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = L2TG(L);
  ctx.owner = foreign_token_owner(L);
  ljt_tg_clear_stopreq(ctx.tg);
  jit_token_rel(g, ctx.owner);

  assert(pthread_create(&th, NULL, publish_stopreq_while_token_waits,
			&ctx) == 0);
  status = luaL_dostring(L, code);
  assert(pthread_join(th, NULL) == 0);
  assert(la_load32_acq(&ctx.saw_native) != 0);
  assert(la_load32_acq(&ctx.signaled) >= 1u);
  assert(la_load32_acq(&ctx.released) != 0);
  assert(jit_token_acq(g) == 0);
  ljt_tg_clear_stopreq(ctx.tg);

  if (status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "unexpected token STOPREQ test error: %s\n",
	    err ? err : "(nil)");
  }
  assert(status == LUA_OK);
  lua_settop(L, 0);
}

static int assert_flush_event_token_owner(lua_State *L)
{
  jit_State *J = L2J(L);
  assert(lj_jit_token_held_l(L, J));
  assert(jit_owner_l_acq(J) == L);
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;

  g = G(L);
  assert(jit_token_acq(g) == 0);
  expect_vmevent_smr_try_drop(L);
  expect_vmevent_owner_and_jit_pointer(L);

  {
    GCtrace trace;
    SnapShot snap;
    uint8_t count;
    memset(&trace, 0, sizeof(trace));
    memset(&snap, 0, sizeof(snap));

    trace_nchild_inc_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 1);
    trace_nchild_dec_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 0);
    trace_nchild_dec_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 0);

    count = 0;
    assert(snap_count_cas_acqrel(&snap, &count, 1) != 0);
    assert(count == 0);
    assert(snap_count_acq(&snap) == 1);

    count = 0;
    assert(snap_count_cas_acqrel(&snap, &count, 2) == 0);
    assert(count == 1);
    assert(snap_count_acq(&snap) == 1);

    snap_count_rel(&snap, SNAPCOUNT_DONE);
    assert(snap_count_acq(&snap) == SNAPCOUNT_DONE);
  }

#if LJ_TARGET_X64 && !LJ_ABI_WIN
  {
    TGState secondary;
    TGState *saved_tg = lj_thr_get_tg();
    assert(g->main_tg != NULL);
    lj_tg_init_thread(g, &secondary, NULL, 0);
    secondary.tid = g->main_tg->tid == 0x7ffffffeu ? 0x7ffffffdu : 0x7ffffffeu;
    secondary.alloc.owner_tid = secondary.tid;
    lj_thr_set_tg(&secondary);
    assert(G2TG(g) == &secondary);
    assert(lj_jit_token_try(g->jitp) != 0);
    assert(jit_token_acq(g) == secondary.tid);
    assert(lj_jit_token_held(g->jitp) != 0);
    lj_jit_token_release(g->jitp);
    assert(jit_token_acq(g) == 0);
    lj_thr_set_tg(saved_tg);
    lj_tg_fini_thread(g, &secondary);
  }
#endif

  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'expected token-owned recording')\n");
  assert(jit_token_acq(g) == 0);

  assert(gc2_n_threads_acq(g) == 1);
  assert(gc2_n_workers_acq(g) == 0);
  expect_loop_xpoll_shape(L, 0);
  {
    uint32_t actions = 0;
    assert(lj_gc2_workers_set_l(L, 1, &actions) == 1);
    assert((actions & LJ_GC2_HS_STOPREQ) == 0);
    assert(gc2_n_workers_acq(g) == 1);
    expect_loop_xpoll_shape(L, 1);
    assert(lj_gc2_workers_set_l(L, 0, &actions) == 1);
    assert(gc2_n_workers_acq(g) == 0);
  }

  /* Sticky-MT full flushes use a safepoint leader, but the public TRACE event
  ** must run afterward on the initiating state while it still owns the recorder
  ** token. Releasing first lets a peer recorder publish J->L and have the event
  ** restore this state over the live owner.
  */
  ljt_lua_dostring(L,
    "local th = require('threading')\n"
    "local w = th.spawn(function() return true end)\n"
    "assert(w:join(5) == true)\n");
  assert(mt_active_acq(g) != 0);

  /* Every iteration uses a fresh, deliberately exhausted coroutine stack.
  ** Race its prepare-time growth against removal and collection of the event
  ** handler's registry root. The SMR load->stack-publication handoff must keep
  ** the ephemeral closure alive even when detach wins immediately afterward.
  */
  lua_pushcfunction(L, force_vmevent_prepare_stack_growth);
  lua_setglobal(L, "lj_m6_force_vmevent_prepare_stack_growth");
  ljt_lua_dostring(L,
    "local th = require('threading')\n"
    "local ready = th.channel(1)\n"
    "local go = th.channel(1)\n"
    "local done = th.channel(1)\n"
    "local rounds = 24\n"
    "local worker = th.spawn(function(ready, go, done, rounds)\n"
    "  for i = 1, rounds do\n"
    "    local hook = function() end\n"
    "    jit.attach(hook, 'trace')\n"
    "    assert(ready:send(i, 5) == true)\n"
    "    local token, ok = go:recv(5)\n"
    "    assert(ok == true and token == i)\n"
    "    jit.attach(hook)\n"
    "    hook = nil\n"
    "    collectgarbage('collect')\n"
    "    assert(done:send(i, 5) == true)\n"
    "  end\n"
    "  return true\n"
    "end, ready, go, done, rounds)\n"
    "for i = 1, rounds do\n"
    "  local co = coroutine.create(function()\n"
    "    return lj_m6_force_vmevent_prepare_stack_growth()\n"
    "  end)\n"
    "  local token, ok = ready:recv(5)\n"
    "  assert(ok == true and token == i,\n"
    "         'ready round ' .. i .. ': ' .. tostring(token))\n"
    "  assert(go:send(i, 5) == true)\n"
    "  local resumed, grew = coroutine.resume(co)\n"
    "  assert(resumed == true, tostring(grew))\n"
    "  assert(grew == true, 'VM-event prepare did not grow the stack')\n"
    "  token, ok = done:recv(5)\n"
    "  assert(ok == true and token == i,\n"
    "         'done round ' .. i .. ': ' .. tostring(token))\n"
    "end\n"
    "assert(worker:join(10) == true)\n");
  lua_pushnil(L);
  lua_setglobal(L, "lj_m6_force_vmevent_prepare_stack_growth");

  lua_pushcfunction(L, assert_flush_event_token_owner);
  lua_setglobal(L, "lj_m6_assert_flush_event_token_owner");
  ljt_lua_dostring(L,
    "local function hook(ev)\n"
    "  if ev == 'flush' then lj_m6_assert_flush_event_token_owner() end\n"
    "end\n"
    "jit.attach(hook, 'trace')\n"
    "jit.flush()\n"
    "jit.attach(hook)\n");
  lua_pushnil(L);
  lua_setglobal(L, "lj_m6_assert_flush_event_token_owner");

  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function lj_m6_busy_tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(lj_m6_busy_tracecount, true)\n"
    "function lj_m6_busy_f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n");
  jit_token_rel(g, 0x7fffffffu);
  ljt_lua_dostring(L,
    "for _ = 1, 40 do assert(lj_m6_busy_f(80) == 3240) end\n"
    "assert(lj_m6_busy_tracecount() == 0, 'busy recorder token must skip tracing')\n");
  assert(jit_token_acq(g) == 0x7fffffffu);

  jit_token_rel(g, 0);
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'recording should resume after token release')\n");
  assert(jit_token_acq(g) == 0);

  expect_flush_waits_for_token(L, "jit.flush()\n", 1);
  expect_flush_waits_for_token(L,
    "local util = require('jit.util')\n"
    "for tr = 1, 32 do\n"
    "  if util.traceinfo(tr) then jit.flush(tr); break end\n"
    "end\n", 0);
  expect_opt_start_waits_for_token(L);
  expect_token_wait_stopreq(L,
    "local ok, err = pcall(jit.flush)\n"
    "assert(not ok and tostring(err):find('VM shutdown', 1, true))\n");
  make_token_flush_trace(L);
  expect_token_wait_stopreq(L,
    "local ok, err = pcall(jit.flush, 1)\n"
    "assert(not ok and tostring(err):find('VM shutdown', 1, true))\n");
  expect_token_wait_stopreq(L,
    "local ok, err = pcall(jit.opt.start, 'hotloop=9')\n"
    "assert(not ok and tostring(err):find('VM shutdown', 1, true))\n");

  expect_gc_trace_free_defers_for_token(L);

  lua_close(L);
  printf("t-jit-token OK: recorder token accepts secondary TGs and owns JIT controls\n");
  return 0;
}
