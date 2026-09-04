/*
** Side publication must finish while a real IDLE reclaimer remains paused.
** Also retain ordinary RETRY event visibility on failed live-body validation.
*/

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
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !defined(LJ_GC2_TEST_HELPERS) && !defined(LJ_TRACE_TEST_HELPERS)
#error "t-jit-stop-admission requires GC2 or trace test helpers"
#endif

/* Private fixture seam; deliberately not part of the runtime header API. */
LJ_FUNC void lj_trace_test_set_stop_hook(
  void (*hook)(jit_State *J, uint32_t stage));

typedef struct StopAdmissionCtx {
  global_State *g;
  GCproto *pt;
  GCtrace *parent;
  GCtrace *root;
  GCtrace *scratch;
  MCode *oldtarget;
  MCode *oldmctop;
  TraceNo traceno;
  TraceNo parentno;
  TraceNo rootno;
  TraceNo oldside;
  ExitNo exitno;
  uint32_t oldnchild;
  uint32_t stage;
  uint32_t fired;
  uint32_t reclaimer_done;
  uint32_t reclaimed;
  uint32_t abort_events;
  uint32_t stop_events;
  uint8_t oldflags;
  uint8_t oldcount;
  int invalidate;
  pthread_t reclaimer;
} StopAdmissionCtx;

static StopAdmissionCtx *active_ctx;

#ifdef LUAJIT_USE_GDBJIT
static int fail_gdb_alloc;
static int failed_gdb_allocs;

/* Link the optional build with -Wl,--wrap=lj_mem_realloc. The first growth
** after checkpoint 4 is gdbjit_newentry's actual metadata allocation call. */
LJ_FUNC void *__real_lj_mem_realloc(lua_State *L, void *p,
                                   GCSize osize, GCSize nsize);
void *__wrap_lj_mem_realloc(lua_State *L, void *p, GCSize osize, GCSize nsize)
{
  if (fail_gdb_alloc && nsize > osize) {
    fail_gdb_alloc = 0;
    failed_gdb_allocs++;
    lj_err_mem(L);
  }
  return __real_lj_mem_realloc(L, p, osize, nsize);
}
#endif

static void *reclaim_main(void *arg)
{
  StopAdmissionCtx *ctx = (StopAdmissionCtx *)arg;
  ctx->reclaimed = lj_gc2_reclaim_retired(
    ctx->g, lj_gc2_retire_epoch(ctx->g) + 1u);
  la_store32_rel(&ctx->reclaimer_done, 1);
  return NULL;
}

static void wait_for_reclaimer(StopAdmissionCtx *ctx)
{
  struct timespec delay = { 0, 1000000 };
  unsigned i;
  for (i = 0; i < 5000; i++) {
    if (lj_gc2_test_idle_reclaim_paused())
      return;
    assert(la_load32_acq(&ctx->reclaimer_done) == 0);
    nanosleep(&delay, NULL);
  }
  assert(!"real IDLE reclaimer did not reach the pause hook");
}

static int trace_event(lua_State *L)
{
  StopAdmissionCtx *ctx = active_ctx;
  const char *event = lua_tostring(L, 1);
  TraceNo tr = (TraceNo)lua_tointeger(L, 2);
  if (!ctx || !ctx->fired || tr != ctx->traceno)
    return 0;
  assert(lj_jit_token_held_l(L, L2J(L)));
  assert(gc2_smr_readers_acq(G(L)) == 0);
  if (strcmp(event, "abort") == 0) {
    assert(lua_tointeger(L, 5) == LJ_TRERR_RETRY);
    assert(L2J(L)->cur.traceno == tr);
    /* Abort IR stays inspectable even though the assembler scratch was retired. */
    assert(L2J(L)->cur.nins > REF_FIRST);
    ctx->abort_events++;
  } else if (strcmp(event, "stop") == 0) {
    assert(trace_runnable_acq(traceref_safe(L2J(L), tr), tr));
    ctx->stop_events++;
  }
  return 0;
}

static void stop_checkpoint(jit_State *J, uint32_t stage)
{
  StopAdmissionCtx *ctx = active_ctx;
  SnapShot *snap;
  if (!ctx || ctx->fired || bc_op(J->cur.startins) != BC_JMP ||
      trace_startpt_acq(&J->cur) != ctx->pt)
    return;
  if (stage == 1) {
    ctx->traceno = J->cur.traceno;
    ctx->scratch = J->curfinal;
    ctx->oldmctop = J->mctop;
    assert(ctx->traceno != 0 && ctx->scratch != NULL);
    ctx->parentno = J->parent;
    ctx->rootno = J->cur.root;
    ctx->exitno = J->exitno;
    assert(ctx->parentno != 0 && ctx->rootno != 0);
    ctx->parent = traceref_safe(J, ctx->parentno);
    ctx->root = traceref_safe(J, ctx->rootno);
    assert(trace_runnable_acq(ctx->parent, ctx->parentno));
    assert(trace_runnable_acq(ctx->root, ctx->rootno));
    assert(ctx->exitno < trace_nsnap_acq(ctx->parent));
    snap = &trace_snap_acq(ctx->parent)[ctx->exitno];
    ctx->oldtarget = trace_exittarget_acq(ctx->parent, ctx->exitno);
    ctx->oldcount = snap_count_acq(snap);
    ctx->oldside = trace_nextside_acq(ctx->root);
    ctx->oldnchild = trace_nchild_acq(ctx->root);
  }
  if (stage != ctx->stage)
    return;
  assert(lj_jit_token_held(J));
  assert(gc2_smr_readers_acq(ctx->g) == 0);
  ctx->fired = 1;
#ifdef LUAJIT_USE_GDBJIT
  if (stage == 4) {
    fail_gdb_alloc = 1;
    return;
  }
#endif
  if (ctx->invalidate) {
    assert(stage == 1);
    ctx->oldflags = la_load8_acq(&ctx->parent->unused1);
    la_store8_rel(&ctx->parent->unused1,
                 ctx->oldflags | TRACE_ENTRY_INVALIDATED);
    return;
  }
  assert(gc2_phase_acq(ctx->g) == LJ_GC2_IDLE);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&ctx->reclaimer, NULL, reclaim_main, ctx) == 0);
  wait_for_reclaimer(ctx);
  assert(gc2_smr_reclaiming_acq(ctx->g) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(gc2_jit_phase_gate_acq(ctx->g) == 0);
  assert(lj_jit_token_held(J));
}

static void call_probe(lua_State *L, int function_index, int flag)
{
  lua_pushvalue(L, function_index);
  lua_pushinteger(L, 80);
  lua_pushboolean(L, flag);
  ljt_lua_pcall(L, 2, 1, "side publication probe");
  assert(lua_tointeger(L, -1) == (flag ? 9720 : 3240));
  lua_pop(L, 1);
}

static void run_case(uint32_t stage, int invalidate)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = L2J(L);
  StopAdmissionCtx ctx;
  GCtrace *child;
  SnapShot *snap;
  int probe, i;
  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.stage = stage;
  ctx.invalidate = invalidate;
  lua_gc(L, LUA_GCSTOP, 0);
  lua_pushcfunction(L, trace_event);
  lua_setglobal(L, "stop_admission_event");
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "jit.attach(stop_admission_event, 'trace')\n");
  ljt_lua_loadstring(L,
    "return function(n, flag)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    if flag then s = s + i * 3 else s = s + i end\n"
    "  end\n"
    "  return s\n"
    "end\n");
  ljt_lua_pcall(L, 0, 1, "load side probe");
  probe = lua_gettop(L);
  ctx.pt = funcproto(funcV(L->top - 1));
  for (i = 0; i < 8; i++)
    call_probe(L, probe, 0);
  assert(proto_trace_acq(ctx.pt) != 0);
  assert(gc2_smr_readers_acq(g) == 0);
  active_ctx = &ctx;
  lj_trace_test_set_stop_hook(stop_checkpoint);
  lj_trace_test_reset_retire_publish_calls();
  alarm(15);
#ifdef LUAJIT_USE_GDBJIT
  if (stage == 4) {
    int status;
    lua_pushvalue(L, probe);
    lua_pushinteger(L, 80);
    lua_pushboolean(L, 1);
    status = lua_pcall(L, 2, 1, 0);
    assert(status == LUA_ERRMEM);
    lua_pop(L, 1);
    assert(failed_gdb_allocs == 1 && fail_gdb_alloc == 0);
  } else
#endif
    call_probe(L, probe, 1);
  lj_trace_test_set_stop_hook(NULL);
  assert(ctx.fired && "probe never reached authentic side publication");
  assert(jit_token_acq(g) == 0);
  assert(J->curfinal == NULL && J->cur.traceno == 0);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(gc2_smr_readers_acq(g) == 0);
  if (!invalidate && stage <= 3) {
    /* Completion and token release precede the fixture's reclaimer release. */
    assert(lj_gc2_test_idle_reclaim_paused());
    assert(la_load32_acq(&ctx.reclaimer_done) == 0);
    assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
  }
  /* The paused fixture owner cannot reclaim while these assertions inspect
  ** exact captured objects; token acquisition independently proves release. */
  assert(lj_jit_token_try_l(L, J));
  child = traceref_safe(J, ctx.traceno);
  snap = &trace_snap_acq(ctx.parent)[ctx.exitno];
  if (stage == 1) {
    assert(child == NULL);
    assert(J->mctop == ctx.oldmctop);
    assert(trace_retired_unpublished_acq(ctx.scratch));
    assert(trace_retired_link_listed_acq(ctx.scratch));
    assert(lj_trace_test_retire_publish_calls() != 0);
    assert(trace_nextside_acq(ctx.root) == ctx.oldside);
    assert(trace_nchild_acq(ctx.root) == ctx.oldnchild);
    assert(trace_exittarget_acq(ctx.parent, ctx.exitno) == ctx.oldtarget);
    assert(snap_count_acq(snap) == ctx.oldcount);
    assert(ctx.stop_events == 0);
    if (invalidate) {
      assert(ctx.abort_events == 1);  /* Ordinary RETRY stays observable. */
      la_store8_rel(&ctx.parent->unused1, ctx.oldflags);
    }
  } else {
    assert(trace_runnable_acq(child, ctx.traceno));
    assert(trace_root_acq(child) == ctx.rootno);
    assert(trace_nextside_acq(ctx.root) == ctx.traceno);
    assert(trace_nchild_acq(ctx.root) == ctx.oldnchild + 1);
    assert(trace_exittarget_acq(ctx.parent, ctx.exitno) == trace_mcode_acq(child));
    assert(snap_count_acq(snap) == SNAPCOUNT_DONE);
#ifdef LUAJIT_USE_GDBJIT
    if (stage == 4) {
      assert(trace_gdbjit_entry_acq(child) == NULL);
      assert(ctx.stop_events == 0 && ctx.abort_events == 0);
    }
#endif
  }
  lj_jit_token_release_l(L, J);
  if (!invalidate && stage <= 3) {
    lj_gc2_test_idle_reclaim_release();
    assert(pthread_join(ctx.reclaimer, NULL) == 0);
    assert(la_load32_acq(&ctx.reclaimer_done) != 0);
    assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_OPEN);
  }
  alarm(0);
  active_ctx = NULL;
  for (i = 0; i < 8; i++)
    call_probe(L, probe, 1);
  lua_close(L);
  printf("side publication stage=%u invalidation=%d OK\n", stage, invalidate);
}

int main(void)
{
  run_case(1, 0);  /* Reclaimer wins before admission: abort before publication. */
  run_case(2, 0);  /* Reclaimer closes after admission, before trace_save(). */
  run_case(3, 0);  /* Reclaimer closes after trace_save(), before parent linking. */
  run_case(1, 1);  /* Validation failure retains the ordinary abort event. */
#ifdef LUAJIT_USE_GDBJIT
  run_case(4, 0);  /* GDB allocation failure cannot unpublish the linked trace. */
#endif
  puts("t-jit-stop-admission OK");
  return 0;
}
