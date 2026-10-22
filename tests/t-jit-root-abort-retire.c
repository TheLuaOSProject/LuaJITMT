/* A root-CAS loser must leave a durable, inspectable cleanup request without
** waiting for a paused IDLE reclaimer after its ordinary ABORT callback. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arena.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "t-jit-root-abort-retire requires GC2 test helpers"
#endif

LJ_FUNC void lj_trace_test_set_stop_hook(
  void (*hook)(jit_State *J, uint32_t stage));

enum { ORDINARY, PAUSE_BEFORE_EVENT, PAUSE_IN_EVENT, FLUSH_IN_EVENT,
       CLOSE_PENDING };
typedef struct RootAbortCtx {
  global_State *g;
  GCproto *pt;
  GCtrace *body;
  BCIns *pc;
  BCIns original;
  TraceNo traceno;
  int mode;
  uint32_t fired, aborted, inspected, stopped, reclaimer_done;
  pthread_t reclaimer;
} RootAbortCtx;
static RootAbortCtx *active_ctx;

static void *reclaim_main(void *arg)
{
  RootAbortCtx *ctx = (RootAbortCtx *)arg;
  (void)lj_gc2_reclaim_retired(ctx->g, lj_gc2_retire_epoch(ctx->g) + 1u);
  la_store32_rel(&ctx->reclaimer_done, 1);
  return NULL;
}

static void pause_reclaimer(RootAbortCtx *ctx)
{
  struct timespec delay = { 0, 1000000 };
  unsigned i;
  assert(gc2_phase_acq(ctx->g) == LJ_GC2_IDLE);
  assert(gc2_smr_readers_acq(ctx->g) == 0);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&ctx->reclaimer, NULL, reclaim_main, ctx) == 0);
  for (i = 0; i < 5000 && !lj_gc2_test_idle_reclaim_paused(); i++) {
    assert(!la_load32_acq(&ctx->reclaimer_done));
    nanosleep(&delay, NULL);
  }
  assert(lj_gc2_test_idle_reclaim_paused());
  assert(gc2_smr_reclaiming_acq(ctx->g) == LJ_GC2_SMR_META_EXCLUSIVE);
}

static void stop_checkpoint(jit_State *J, uint32_t stage)
{
  RootAbortCtx *ctx = active_ctx;
  if (!ctx || trace_startpt_acq(&J->cur) != ctx->pt ||
      bc_op(J->cur.startins) != BC_LOOP)
    return;
  if (stage == 1 && !ctx->fired) {
    ctx->fired = 1;
    ctx->body = J->curfinal;
    ctx->traceno = J->cur.traceno;
    ctx->pc = (BCIns *)trace_startpc_acq(&J->cur);
    ctx->original = J->cur.startins;
    assert(ctx->body && ctx->traceno);
    lj_bc_test_force_publish_cas_collision(
      BCINS_AD(BC_ILOOP, bc_a(ctx->original), bc_d(ctx->original)));
  }
  if (stage == 3 && ctx->mode == PAUSE_BEFORE_EVENT)
    pause_reclaimer(ctx);
}

static void inspect_event(lua_State *L, TraceNo tr)
{
  lua_getglobal(L, "root_abort_inspect");
  lua_pushinteger(L, tr);
  ljt_lua_pcall(L, 1, 0, "ordinary root ABORT inspection");
}

static int trace_event(lua_State *L)
{
  RootAbortCtx *ctx = active_ctx;
  const char *event = lua_tostring(L, 1);
  TraceNo tr = (TraceNo)lua_tointeger(L, 2);
  if (!ctx || !ctx->fired || tr != ctx->traceno)
    return 0;
  if (strcmp(event, "stop") == 0) {
    ctx->stopped++;
    return 0;
  }
  if (strcmp(event, "abort") != 0)
    return 0;
  assert(lua_tointeger(L, 5) == LJ_TRERR_RETRY);
  assert(lj_jit_token_held_l(L, L2J(L)));
  assert(gc2_smr_readers_acq(ctx->g) == 0);
  inspect_event(L, tr);
  ctx->inspected++;
  ctx->aborted++;
  /* A same-owner root scan during the event may preserve the request but
  ** must not consume it before ordinary inspection is finished. */
  assert(lj_trace_markvecs(ctx->g, 1) == 0);
  inspect_event(L, tr);
  if (ctx->mode == PAUSE_IN_EVENT) {
    pause_reclaimer(ctx);  /* The old post-callback read_enter hangs here. */
  } else if (ctx->mode == FLUSH_IN_EVENT) {
    ljt_lua_dostring(L,
      "collectgarbage('collect')\n"
      "jit.flush()\n"
      "collectgarbage('collect')\n");
    /* Reacquire body authority after arbitrary Lua. The original slot may
    ** already be absent; never dereference the captured source pointer. */
    assert(lj_gc2_smr_read_try(ctx->g));
    assert(traceref_safe(L2J(L), tr) != ctx->body ||
           !trace_runnable_acq(traceref_safe(L2J(L), tr), tr));
    lj_gc2_smr_read_leave(ctx->g);
  }
  return 0;
}

static lua_State *new_state(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  lua_gc(L, LUA_GCSTOP, 0);  /* Schedule the collector explicitly in this race. */
  lua_pushcfunction(L, trace_event);
  lua_setglobal(L, "root_abort_event");
  ljt_lua_dostring(L,
    "local u = require('jit.util')\n"
    "function root_abort_inspect(tr)\n"
    "  local info = assert(u.traceinfo(tr))\n"
    "  assert(info.nins > 0 and u.traceir(tr, 1))\n"
    "end\n"
    "jit.off(root_abort_inspect, true)\n"
    "jit.opt.start('hotloop=1', 'hotexit=100000', 'maxtrace=8')\n"
    "jit.attach(root_abort_event, 'trace')\n");
  return L;
}

static int new_probe(lua_State *L, RootAbortCtx *ctx)
{
  ljt_lua_loadstring(L,
    "local payload = { offset = 3 }\n"
    "return function(n)\n"
    "  local offset = payload.offset\n"
    "  local i, s = 0, 0\n"
    "  while i < n do i = i + 1; s = s + i + offset end\n"
    "  return s\n"
    "end\n");
  ljt_lua_pcall(L, 0, 1, "load root CAS probe");
  ctx->g = G(L);
  ctx->pt = funcproto(funcV(L->top - 1));
  return lua_gettop(L);
}

static void call_probe(lua_State *L, int index)
{
  lua_pushvalue(L, index);
  lua_pushinteger(L, 80);
  ljt_lua_pcall(L, 1, 1, "root CAS probe");
  assert(lua_tointeger(L, -1) == 3480);
  lua_pop(L, 1);
}

static void collide(lua_State *L, RootAbortCtx *ctx, int probe)
{
  unsigned i;
  active_ctx = ctx;
  lj_trace_test_set_stop_hook(stop_checkpoint);
  for (i = 0; i < 16 && !ctx->fired; i++)
    call_probe(L, probe);
  lj_trace_test_set_stop_hook(NULL);
  assert(ctx->fired && !lj_bc_test_publish_cas_collision_pending());
  assert(ctx->stopped == 0);
  assert(jit_token_acq(G(L)) == 0);
  assert(lj_trace_state_load(L2J(L)) == LJ_TRACE_IDLE);
  assert(L2J(L)->curfinal == NULL && L2J(L)->cur.traceno == 0);
  assert(gc2_smr_readers_acq(G(L)) == 0);
  assert(bc_op((BCIns)la_load32_acq((uint32_t *)ctx->pc)) == BC_ILOOP);
  active_ctx = NULL;
}

static GCtrace *pending_body(lua_State *L, RootAbortCtx *ctx)
{
  GCtrace *T = traceref_safe(L2J(L), ctx->traceno);
  assert(T == ctx->body);
  assert(trace_traceno_acq(T) == ctx->traceno);
  assert(trace_retire_pending_acq(T));
  assert(!trace_runnable_acq(T, ctx->traceno));
  assert(la_load64_acq(&T->retire_epoch) == 0);
  assert(!trace_retired_link_listed_acq(T));
  return T;
}

static void collect_to_drain(lua_State *L)
{
  unsigned i;
  lua_gc(L, LUA_GCRESTART, 0);
  for (i = 0; i < LJ_FLUSH_EPOCHS + 8u; i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  assert(lj_gc2_smr_read_try(G(L)));
  assert(trace_retired_head_acq(L2J(L)) == NULL);
  lj_gc2_smr_read_leave(G(L));
  lua_gc(L, LUA_GCSTOP, 0);
}

static void run_case(int mode)
{
  lua_State *L = new_state();
  global_State *g = G(L);
  jit_State *J = L2J(L);
  RootAbortCtx ctx;
  int probe;
  memset(&ctx, 0, sizeof(ctx));
  ctx.mode = mode;
  probe = new_probe(L, &ctx);
  alarm(15);
  collide(L, &ctx, probe);
  if (mode == PAUSE_BEFORE_EVENT || mode == PAUSE_IN_EVENT) {
    assert(lj_gc2_test_idle_reclaim_paused());
    assert(!la_load32_acq(&ctx.reclaimer_done));
    assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
    /* A token plus the stopped fixture reclaimer pins this exact source.
    ** The production scan itself must refuse before inspecting any slot. */
    assert(lj_jit_token_try_l(L, J));
    (void)pending_body(L, &ctx);
    assert(lj_trace_markvecs(g, 1) == 0);
    assert(gc2_smr_readers_acq(g) == 0);
    lj_jit_token_release_l(L, J);
    lj_gc2_test_idle_reclaim_release();
    assert(pthread_join(ctx.reclaimer, NULL) == 0);
    assert(la_load32_acq(&ctx.reclaimer_done));
  }
  if (mode != PAUSE_BEFORE_EVENT)
    assert(ctx.aborted == 1 && ctx.inspected == 1);
  if (mode == CLOSE_PENDING) {
    /* Close consumes the existing ordinary-public-slot path at epoch zero. */
    lua_close(L);
    alarm(0);
    puts("root ABORT terminal pending-slot cleanup OK");
    return;
  }
  if (mode != FLUSH_IN_EVENT) {
    GCtrace *T;
    GCArena *a;
    if (mode == ORDINARY) {
      lua_State *other = luaL_newstate();
      assert(other != NULL);
      assert(lj_gc2_smr_read_tracked_try(G(other)));
      assert(lj_trace_markvecs(g, 1) == 0);
      assert(gc2_smr_readers_acq(g) == 0);
      lj_gc2_smr_read_leave(G(other));
      lua_close(other);
    }
    assert(lj_gc2_smr_read_tracked_try(g));
    T = pending_body(L, &ctx);
    a = lj_arena_of(T);
    assert(!lj_arena_ishuge(a));
    lj_arena_bm_clear(a->mark, lj_arena_cellof(T));
    assert(!lj_arena_ishuge(lj_arena_of(ctx.pt)));
    lj_arena_bm_clear(lj_arena_of(ctx.pt)->mark, lj_arena_cellof(ctx.pt));
    /* A competing token cannot drop the pending semantic graph. */
    jit_owner_test_rel(g, 0x7fffffffu, 0);
    assert(lj_trace_markvecs(g, 1) == 0);
    assert(trace_retire_pending_acq(T));
    assert(lj_gc2_ismarked(g, obj2gco(T)) > 0);
    assert(lj_gc2_ismarked(g, obj2gco(ctx.pt)) > 0);
    jit_owner_test_rel(g, 0, 0);
    /* A closer observed after tracked admission cannot trap nested readers.
    ** This gate schedule is a local white-box check; the real reclaimer
    ** schedule above independently proves public-path completion. */
    gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_META_EXCLUSIVE);
    assert(lj_trace_markvecs(g, 1) == 1);
    gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
    assert(!trace_retire_pending_acq(T));
    assert(la_load64_acq(&T->retire_epoch) != 0);
    assert(trace_retired_link_listed_acq(T));
    assert(traceref_safe(J, ctx.traceno) != T);
    lj_gc2_smr_read_leave(g);
  }
  collect_to_drain(L);
  /* Reuse the original number through authentic ordinary recording. The old
  ** abort invocation has no pointer cleanup left that can retire this body. */
  bc_publish(ctx.pc, ctx.original);
  for (unsigned i = 0; i < 32; i++)
    call_probe(L, probe);
  assert(lj_gc2_smr_read_try(g));
  assert(trace_runnable_acq(traceref_safe(J, ctx.traceno), ctx.traceno));
  assert(!trace_retire_pending_acq(traceref_safe(J, ctx.traceno)));
  lj_gc2_smr_read_leave(g);
  lua_close(L);
  alarm(0);
  printf("root ABORT mode=%d OK\n", mode);
}

static void test_capacity(void)
{
  lua_State *L = new_state();
  RootAbortCtx ctx[10];
  unsigned round, i;
  alarm(20);
  for (round = 0; round < 3; round++) {
    memset(ctx, 0, sizeof(ctx));
    for (i = 0; i < 10; i++) {
      int probe = new_probe(L, &ctx[i]);
      collide(L, &ctx[i], probe);
      assert(lj_gc2_smr_read_try(G(L)));
      (void)pending_body(L, &ctx[i]);
      for (unsigned j = 0; i < 8 && j < i; j++)
        assert(ctx[j].traceno != ctx[i].traceno);
      lj_gc2_smr_read_leave(G(L));
    }
    /* Exceed maxtrace=8 to exercise the ordinary capacity-triggered flush.
    ** Real collection then consumes the remaining pending requests and drains
    ** all retired bodies. Repeating proves there is no permanent slot leak. */
    collect_to_drain(L);
    assert(lj_gc2_smr_read_try(G(L)));
    for (i = 0; i < 10; i++)
      assert(traceref_safe(L2J(L), ctx[i].traceno) == NULL);
    lj_gc2_smr_read_leave(G(L));
    lua_settop(L, 0);
  }
  lua_close(L);
  alarm(0);
  puts("root ABORT capacity and eventual drain OK");
}

int main(void)
{
  run_case(PAUSE_IN_EVENT);  /* First: exact original-wait negative control. */
  run_case(PAUSE_BEFORE_EVENT);
  run_case(ORDINARY);
  run_case(FLUSH_IN_EVENT);
  run_case(CLOSE_PENDING);
  test_capacity();
  puts("t-jit-root-abort-retire OK");
  return 0;
}
