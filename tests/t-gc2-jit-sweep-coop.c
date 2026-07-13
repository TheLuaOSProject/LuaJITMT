/*
** x64 native trace execution between bounded GC2 SWEEP reclaim quanta.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

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
static uint32_t probe_hits;

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

typedef struct AsyncCloseCtx {
  global_State *g;
  uint32_t saw_active;
  uint32_t step;
  uint64_t elapsed_ns;
} AsyncCloseCtx;

static void *async_close_main(void *arg)
{
  AsyncCloseCtx *ctx = (AsyncCloseCtx *)arg;
  uint64_t start;
  (void)lj_thr_sleep_ns(NULL, 1000000);
  ctx->saw_active = (uint32_t)lj_tg_any_jit_active(ctx->g);
  start = lj_thr_now_ns();
  ctx->step = lj_gc2_worker_drain(ctx->g, LJ_GC2_SWEEP_BATCH);
  ctx->elapsed_ns = lj_thr_now_ns() - start;
  return NULL;
}

static void publish_probe_ptr(lua_State *L, const char *name, void *ptr)
{
  lua_pushlightuserdata(L, ptr);
  lua_setglobal(L, name);
}

static void drive_to_open_sweep(lua_State *L, global_State *g)
{
  uint32_t i;
  lua_gc(L, LUA_GCRESTART, 0);
  for (i = 0; i < 1000000u; i++) {
    (void)lua_gc(L, LUA_GCSTEP, 1);
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	gc2_sweep_bridge_ready_acq(g) != 0 &&
	gc2_jit_phase_gate_acq(g) != 0)
      return;
  }
  fputs("GC2 did not reach an open cooperative SWEEP window\n", stderr);
  assert(0);
}

static void drive_to_idle(lua_State *L, global_State *g)
{
  uint32_t i;
  for (i = 0; i < 2000000u && gc2_phase_acq(g) != LJ_GC2_IDLE; i++)
    (void)lua_gc(L, LUA_GCSTEP, 1);
  if (gc2_phase_acq(g) != LJ_GC2_IDLE) {
    fprintf(stderr, "cooperative SWEEP remained in phase %u\n",
	    gc2_phase_acq(g));
    assert(0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
#if defined(LJ_GC2_TEST_HELPERS)
  uint64_t hard_check0, sweep_runs0, sweep_arenas0;
#else
  uint64_t sweep_runs0, sweep_arenas0;
#endif
  uint32_t worker_actions = 0;
  AsyncCloseCtx closectx;
  pthread_t closer;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(g != NULL);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(lj_gc2_workers_set_l(L, 0, &worker_actions) == 1);
  assert(gc2_n_workers_acq(g) == 0);
  publish_probe_ptr(L, "__gc2_sweep_phase_ptr", &g->gc2.phase);
  publish_probe_ptr(L, "__gc2_sweep_gate_ptr", &g->gc2.jit_phase_gate);
  publish_probe_ptr(L, "__gc2_sweep_vmstate_ptr", &G2TG(g)->vmstate);
  publish_probe_ptr(L, "__gc2_sweep_hits_ptr", &probe_hits);

  /* Compile a real allocating loop in IDLE. The FFI probe executes as part of
  ** the trace and uses branch-free volatile loads of phase/gate/vmstate to
  ** distinguish native execution from interpreted fallback without adding
  ** telemetry to the production entry path. */
  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local ffi = require('ffi')\n"
    "local util = require('jit.util')\n"
    "local bit = require('bit')\n"
    "local phase = ffi.cast('volatile uint32_t *', __gc2_sweep_phase_ptr)\n"
    "local gate = ffi.cast('volatile uint32_t *', __gc2_sweep_gate_ptr)\n"
    "local vmstate = ffi.cast('volatile int32_t *', __gc2_sweep_vmstate_ptr)\n"
    "local hits = ffi.cast('volatile uint32_t *', __gc2_sweep_hits_ptr)\n"
    "function __gc2_sweep_coop(n, seed)\n"
    "  local t = {}\n"
    "  for i = 1, n do\n"
    /* vmstate is non-negative only in compiled code. is_sweep is a branchless
    ** uint32 equality test, so recording in IDLE does not install a phase guard
    ** which would force the active-phase call back to the interpreter. */
    "    local native = bit.rshift(bit.bnot(vmstate[0]), 31)\n"
    "    local x = bit.bxor(phase[0], 3)\n"
    "    local is_sweep = bit.rshift(bit.bnot(bit.bor(x, -x)), 31)\n"
    "    hits[0] = hits[0] + bit.band(native, is_sweep, gate[0])\n"
    "    t['coop' .. (seed + i)] = i\n"
    "  end\n"
    "  return t\n"
    "end\n"
    "assert(type(__gc2_sweep_coop(400, 0)) == 'table')\n"
    "assert(util.traceinfo(1), 'cooperative SWEEP loop did not trace')\n"
    "function __gc2_sweep_pure(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x + i end\n"
    "  return x\n"
    "end\n"
    "assert(__gc2_sweep_pure(400) == 80200)\n"
    "assert(util.traceinfo(2), 'pure cooperative loop did not trace')\n"
    /* Build unreachable old-generation work while automatic collection is
    ** stopped, so the following explicit cycle has physical reclaim to do. */
    "for round = 1, 20 do\n"
    "  local dead = {}\n"
    "  for i = 1, 2500 do dead[i] = { round, i, 'dead' .. i } end\n"
    "end\n");

  la_store32_rel(&probe_hits, 0);
  drive_to_open_sweep(L, g);
  assert(lj_gc2_jit_entry_open(g));
  sweep_runs0 = gc2_sweep_owner_runs_acq(g);
  sweep_arenas0 = gc2_sweep_owner_arenas_acq(g);

#if defined(LJ_GC2_TEST_HELPERS)
  /* Attribute the close to the common allocation-accounting checkpoint, not
  ** to a worker or the later asm_gc_check path. Seed a due hard cadence and a
  ** one-byte-short TG batch while entry is observably open. The real traced
  ** table/string allocations below must flush that batch with jit_base live,
  ** close the gate, and advance hard_check_bytes before returning. */
  la_store64_rel(&g->gc2.alloc_since_trigger,
		 LJ_GC2_ACCT_FLUSH * 2u);
  la_store64_rel(&g->gc2.hard_bytes, LJ_GC2_ACCT_FLUSH);
  lj_gc2_hard_check_store(g, LJ_GC2_ACCT_FLUSH * 2u);
  la_store64_rel(&tg->local_total, LJ_GC2_ACCT_FLUSH - 1u);
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  hard_check0 = lj_gc2_hard_check_load(g);
  lj_gc2_test_jit_sweep_checkpoint_reset();
  assert(gc2_jit_phase_gate_acq(g) != 0);
#endif

  lua_getglobal(L, "__gc2_sweep_coop");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 80000);
  lua_pushinteger(L, 1000000);
  ljt_lua_pcall(L, 2, 1, "cooperative SWEEP trace");
  assert(lua_istable(L, -1));  /* Keep every new key rooted through close. */
#if defined(LJ_GC2_TEST_HELPERS)
  assert(lj_gc2_test_jit_sweep_checkpoint_closes() != 0);
  assert(lj_gc2_hard_check_load(g) > hard_check0);
#endif
  if (la_load32_acq(&probe_hits) == 0) {
    fputs("compiled loop never executed while GC2 SWEEP was active\n", stderr);
    assert(0);
  }

  drive_to_idle(L, g);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(gc2_sweep_owner_runs_acq(g) > sweep_runs0);
  assert(gc2_sweep_owner_arenas_acq(g) > sweep_arenas0);
  lua_pop(L, 1);

  /* A foreign collector closes the gate while a pure non-allocating loop is
  ** on-trace. The close attempt must return immediately (including when the
  ** active TG is the collector caller in another embedding), XPOLL must force
  ** the trace through its ordinary snapshot, and a later owner must finish the
  ** cycle without a synchronous EXIT_TRACES wait. */
  /* Worker activation flushes the earlier no-XPOLL traces and makes this
  ** subsequent x64 recording include the asynchronous gate poll. Stop the
  ** worker after recording so the raw closer owns a deterministic close
  ** attempt; the conservative XPOLL trace remains valid after deactivation. */
  lua_gc(L, LUA_GCSTOP, 0);
  lj_gc2_cycle_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_gc2_workers_set_l(L, 1, &worker_actions) == 1);
  assert((worker_actions & LJ_GC2_HS_STOPREQ) == 0);
  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1')\n"
    "local util = require('jit.util')\n"
    "function __gc2_sweep_pure(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x + i end\n"
    "  return x\n"
    "end\n"
    "for _ = 1, 20 do assert(__gc2_sweep_pure(400) == 80200) end\n"
    "local found = false\n"
    "for i = 1, 100 do if util.traceinfo(i) then found = true end end\n"
    "assert(found, 'XPOLL pure loop did not trace')\n");
  assert(has_xpoll_trace(g));
  assert(lj_gc2_workers_set_l(L, 0, &worker_actions) == 1);
  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "for round = 1, 20 do\n"
    "  local dead = {}\n"
    "  for i = 1, 2500 do dead[i] = { round, i, 'puredead' .. i } end\n"
    "end\n");
  drive_to_open_sweep(L, g);
  sweep_runs0 = gc2_sweep_owner_runs_acq(g);
  closectx.g = g;
  closectx.saw_active = 0;
  closectx.step = ~(uint32_t)0;
  closectx.elapsed_ns = ~(uint64_t)0;
  assert(pthread_create(&closer, NULL, async_close_main, &closectx) == 0);
  lua_getglobal(L, "__gc2_sweep_pure");
  lua_pushinteger(L, 50000000);
  ljt_lua_pcall(L, 1, 1, "pure cooperative SWEEP trace");
  assert(lua_isnumber(L, -1));
  lua_pop(L, 1);
  assert(pthread_join(closer, NULL) == 0);
  assert(closectx.saw_active != 0);
  assert(closectx.step == 0);
  assert(closectx.elapsed_ns < 50000000u);
  drive_to_idle(L, g);
  assert(gc2_sweep_owner_runs_acq(g) > sweep_runs0);

  lua_pushnil(L);
  lua_setglobal(L, "__gc2_sweep_coop");
  lua_pushnil(L);
  lua_setglobal(L, "__gc2_sweep_pure");
  lua_pushnil(L); lua_setglobal(L, "__gc2_sweep_phase_ptr");
  lua_pushnil(L); lua_setglobal(L, "__gc2_sweep_gate_ptr");
  lua_pushnil(L); lua_setglobal(L, "__gc2_sweep_vmstate_ptr");
  lua_pushnil(L); lua_setglobal(L, "__gc2_sweep_hits_ptr");
  lua_close(L);
  puts("t-gc2-jit-sweep-coop OK: x64 trace ran during SWEEP and bounded "
       "reclaim reached IDLE");
  return 0;
}
#else
int main(void)
{
  puts("t-gc2-jit-sweep-coop SKIP: cooperative entry is x64-only");
  return 0;
}
#endif
