/*
** Deterministic IDLE retired-body reclaim versus late native-entry regression.
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
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !defined(LJ_GC2_TEST_HELPERS) && !defined(LJ_TRACE_TEST_HELPERS)
#error "t-jit-idle-reclaim-entry requires GC2 or trace test helpers"
#endif

#if LJ_TARGET_X64
typedef struct IdleReclaimCtx {
  global_State *g;
  uint64_t epoch;
  uint32_t reclaimed;
  uint32_t done;
} IdleReclaimCtx;

static uint32_t native_hits;

typedef struct PatchedPC {
  BCIns *pc;
  BCIns original;
} PatchedPC;

static void *idle_reclaim_main(void *arg)
{
  IdleReclaimCtx *ctx = (IdleReclaimCtx *)arg;
  ctx->reclaimed = lj_gc2_reclaim_retired(ctx->g, ctx->epoch);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void publish_ptr(lua_State *L, const char *name, void *p)
{
  lua_pushlightuserdata(L, p);
  lua_setglobal(L, name);
}

static void wait_for_idle_reclaim_pause(IdleReclaimCtx *ctx)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_gc2_test_idle_reclaim_paused())
      return;
    assert(la_load32_acq(&ctx->done) == 0);
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"IDLE reclaimer did not reach post-JIT-quiescence hook");
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static int proto_has_op(GCproto *pt, BCOp want)
{
  BCIns *bc = proto_bc(pt);
  BCPos i;
  for (i = 0; i < pt->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i])) == want)
      return 1;
  return 0;
}

static PatchedPC patch_first_op(lua_State *L, const char *name, BCOp want)
{
  GCproto *pt = global_proto(L, name);
  BCIns *bc = proto_bc(pt);
  PatchedPC patch = { NULL, 0 };
  BCPos i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == want) {
      patch.pc = &bc[i];
      patch.original = ins;
      proto_jit_startins_rel(pt, patch.pc, ins);
      bc_publish(patch.pc, BCINS_AD(BC_JLOOP, bc_a(ins), 1));
      return patch;
    }
  }
  assert(!"requested bytecode opcode not found");
  return patch;
}

static void restore_patch(PatchedPC *patch)
{
  assert(patch && patch->pc && patch->original != 0);
  bc_publish(patch->pc, patch->original);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  IdleReclaimCtx ctx;
  PatchedPC loop_patch, ret_patch, itern_patch;
  pthread_t reclaimer;
  int cache_base, jfuncf_idx, loop_idx, ret_idx, itern_idx, itern_input_idx;
  int probe_idx, generic_idx, generic_input_idx;

  assert(g != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  publish_ptr(L, "__idle_reclaim_vmstate", &G2TG(g)->vmstate);
  publish_ptr(L, "__idle_reclaim_hits", &native_hits);
  /* The native-entry probe intentionally uses FFI/cdata metadata and therefore
  ** runs only while the metadata SMR writer can make progress. The separate
  ** closed-window numeric and generic traces below use scalar/upvalue state;
  ** an unrelated retained metadata lookup would correctly wait for the test's
  ** deliberately frozen exclusive writer and obscure the JIT-entry proof. */
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1')\n"
    "local ffi = require('ffi')\n"
    "local bit = require('bit')\n"
    "local util = require('jit.util')\n"
    "local vmstate = ffi.cast('volatile int32_t *', __idle_reclaim_vmstate)\n"
    "local hits = ffi.cast('volatile uint32_t *', __idle_reclaim_hits)\n"
    "do\n"
    "  local f = function(n) local x=0; for i=1,n do x=x+i end; return x end\n"
    "  local loaded = assert(loadstring(string.dump(f)))\n"
    "  for _ = 1, 20 do assert(loaded(40) == 820) end\n"
    "end\n"
    "function __idle_reclaim_probe(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do\n"
    "    hits[0] = hits[0] + bit.rshift(bit.bnot(vmstate[0]), 31)\n"
    "    x = x + i\n"
    "  end\n"
    "  return x\n"
    "end\n"
    "for _ = 1, 20 do assert(__idle_reclaim_probe(400) == 80200) end\n"
    "function __idle_reclaim_closed_probe(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x + i end\n"
    "  return x\n"
    "end\n"
    "for _ = 1, 20 do assert(__idle_reclaim_closed_probe(400) == 80200) end\n"
    "local function idle_generic_iter(limit, i)\n"
    "  i = i + 1\n"
    "  if i <= limit then return i, i * 11 end\n"
    "end\n"
    "generic_input = 5\n"
    "function __idle_reclaim_generic(limit)\n"
    "  local n, x = 0, 0\n"
    "  for _, v in idle_generic_iter, limit, 0 do\n"
    "    n, x = n + 1, x + v\n"
    "  end\n"
    "  return n, x\n"
    "end\n"
    "for _ = 1, 20 do\n"
    "  local n, x = __idle_reclaim_generic(generic_input)\n"
    "  assert(n == 5 and x == 165)\n"
    "end\n"
    "function __idle_reclaim_jfuncf(x) return x * 3 + 1 end\n"
    "for i = 1, 200 do assert(__idle_reclaim_jfuncf(i) == i * 3 + 1) end\n"
    "function __idle_shadow_loop(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i end\n"
    "  return i, x\n"
    "end\n"
    "jit.off(__idle_shadow_loop, true)\n"
    "function __idle_shadow_ret(x) return x + 7 end\n"
    "jit.off(__idle_shadow_ret, true)\n"
    "idle_shadow_input = {11, 22, 33, 44, 55}\n"
    "local idle_real_next = next\n"
    "local next = idle_real_next\n"
    "function idle_next_wrapper(t, k) return idle_real_next(t, k) end\n"
    "jit.off(idle_next_wrapper, true)\n"
    "function __idle_shadow_set_next(f) next = f end\n"
    "function __idle_shadow_itern(t)\n"
    "  local n, x = 0, 0\n"
    "  for _, v in next, t do n, x = n + 1, x + v end\n"
    "  return n, x\n"
    "end\n"
    "jit.off(__idle_shadow_itern, true)\n"
    "assert(util.traceinfo(1) and util.traceinfo(2),\n"
    "       'IDLE numeric/generic entry probes did not trace')\n");
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(la_load32_acq(&native_hits) != 0);
  {
    int i;
    for (i = 1; i <= 200; i++) {
      lua_getglobal(L, "__idle_reclaim_jfuncf");
      lua_pushinteger(L, i);
      ljt_lua_pcall(L, 1, 1, "JFUNCF C-entry warmup");
      assert(lua_tointeger(L, -1) == i * 3 + 1);
      lua_pop(L, 1);
    }
  }
  assert(proto_has_op(global_proto(L, "__idle_reclaim_jfuncf"), BC_JFUNCF));
  loop_patch = patch_first_op(L, "__idle_shadow_loop", BC_LOOP);
  if (proto_has_op(global_proto(L, "__idle_shadow_ret"), BC_RET1))
    ret_patch = patch_first_op(L, "__idle_shadow_ret", BC_RET1);
  else if (proto_has_op(global_proto(L, "__idle_shadow_ret"), BC_RET))
    ret_patch = patch_first_op(L, "__idle_shadow_ret", BC_RET);
  else
    ret_patch = patch_first_op(L, "__idle_shadow_ret", BC_RET0);
  assert(proto_has_op(global_proto(L, "__idle_shadow_itern"), BC_ISNEXT));
  itern_patch = patch_first_op(L, "__idle_shadow_itern", BC_ITERN);
  if (!lj_gc2_test_idle_reclaim_enter(g)) {
    fprintf(stderr,
      "idle reclaim preflight failed: state=%u phase=%u worker=%u assist=%u "
      "weakdrain=%u weakwrite=%u veto=%d gate=%u active=%d\n",
      (unsigned)g->gc.state, gc2_phase_acq(g), gc2_worker_active_acq(g),
      gc2_assist_active_acq(g), gc2_weak_drain_active_acq(g),
      gc2_weak_write_active_acq(g), lj_gc2_activation_reclaim_veto(g),
      gc2_jit_phase_gate_acq(g), lj_tg_any_jit_active(g));
    assert(0);
  }
  lj_gc2_test_idle_reclaim_leave(g);

  /* Retained table lookup now takes a short SMR reader while copying a global
  ** value. Preload every value needed below before the test hook freezes the
  ** exclusive reclaimer; otherwise lua_getglobal() would correctly wait for
  ** the deliberately paused writer and the fixture would deadlock itself
  ** before exercising closed-gate bytecode recovery. Keep the cached values on
  ** the Lua stack as ordinary roots and duplicate them for each call. */
  cache_base = lua_gettop(L);
  lua_getglobal(L, "__idle_reclaim_jfuncf");
  jfuncf_idx = lua_gettop(L);
  lua_getglobal(L, "__idle_shadow_loop");
  loop_idx = lua_gettop(L);
  lua_getglobal(L, "__idle_shadow_ret");
  ret_idx = lua_gettop(L);
  lua_getglobal(L, "__idle_shadow_itern");
  itern_idx = lua_gettop(L);
  lua_getglobal(L, "idle_shadow_input");
  itern_input_idx = lua_gettop(L);
  lua_getglobal(L, "__idle_reclaim_closed_probe");
  probe_idx = lua_gettop(L);
  assert(lua_isfunction(L, jfuncf_idx));
  assert(lua_isfunction(L, loop_idx));
  assert(lua_isfunction(L, ret_idx));
  assert(lua_isfunction(L, itern_idx));
  assert(lua_istable(L, itern_input_idx));
  assert(lua_isfunction(L, probe_idx));

  ctx.g = g;
  ctx.epoch = lj_gc2_retire_epoch(g) + 1u;
  ctx.reclaimed = ~(uint32_t)0;
  ctx.done = 0;
  la_store32_rel(&native_hits, 0);
  gc2_jit_sweep_displaced_rel(g, 0);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&reclaimer, NULL, idle_reclaim_main, &ctx) == 0);
  wait_for_idle_reclaim_pause(&ctx);

  /* This hook is immediately after the reclaimer's zero-active sample and
  ** immediately before its trace/mcode retired-slot release pass. A real
  ** BC_JLOOP entry attempt must remain interpreted while the owned gate is
  ** closed. Prototype-owned startins recovery must let the interpreter finish
  ** every iteration before the paused SMR writer is released. The scalar
  ** closed probe's displaced counter is the authoritative native-entry veto;
  ** the FFI/vmstate probe independently proves native execution before and
  ** after this artificial pause. */
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(!lj_gc2_jit_entry_open(g));
  gc2_jit_sweep_displaced_rel(g, 0);
  lua_pushvalue(L, jfuncf_idx);
  lua_pushinteger(L, 37);
  ljt_lua_pcall(L, 1, 1, "closed IDLE JFUNCF entry");
  assert(lua_tointeger(L, -1) == 112);
  lua_pop(L, 1);
  assert(gc2_jit_sweep_displaced_acq(g) != 0);

  gc2_jit_sweep_displaced_rel(g, 0);
  lua_pushvalue(L, loop_idx);
  lua_pushinteger(L, 1000);
  ljt_lua_pcall(L, 1, 2, "closed IDLE LOOP shadow");
  assert(lua_tointeger(L, -2) == 1000);
  assert(lua_tointeger(L, -1) == 499500);
  lua_pop(L, 2);
  assert(gc2_jit_sweep_displaced_acq(g) != 0);

  gc2_jit_sweep_displaced_rel(g, 0);
  lua_pushvalue(L, ret_idx);
  lua_pushinteger(L, 35);
  ljt_lua_pcall(L, 1, 1, "closed IDLE RET shadow");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  assert(gc2_jit_sweep_displaced_acq(g) != 0);

  gc2_jit_sweep_displaced_rel(g, 0);
  lj_trace_test_force_startins_retry(1);
  lua_pushvalue(L, itern_idx);
  lua_pushvalue(L, itern_input_idx);
  ljt_lua_pcall(L, 1, 2, "closed IDLE ITERN shadow");
  assert(lua_tointeger(L, -2) == 5);
  assert(lua_tointeger(L, -1) == 165);
  lua_pop(L, 2);
  assert(gc2_jit_sweep_displaced_acq(g) != 0);
  assert(bc_op((BCIns)la_load32_acq((const uint32_t *)itern_patch.pc)) ==
	 BC_JLOOP);

  gc2_jit_sweep_displaced_rel(g, 0);
  lj_trace_test_force_startins_retry(1);
  lua_pushvalue(L, probe_idx);
  lua_pushinteger(L, 20000);
  ljt_lua_pcall(L, 1, 1, "closed IDLE reclaim entry");
  assert(lua_tonumber(L, -1) == 200010000.0);
  lua_pop(L, 1);
  assert(gc2_jit_sweep_displaced_acq(g) != 0);
  assert(la_load32_acq(&native_hits) == 0);
  assert(!lj_tg_any_jit_active(g));
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(gc2_smr_reclaiming_acq(g) != 0);
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(reclaimer, NULL) == 0);
  assert(la_load32_acq(&ctx.done) != 0);
  assert(gc2_smr_reclaiming_acq(g) == 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  lua_settop(L, cache_base);

  /* A failed ISNEXT must take a trace-body lease before deciding whether the
  ** observed JLOOP still names the exact live ITERN generation and before
  ** publishing JLOOP -> ITERC. Do that lease-dependent despecialization only
  ** after the deliberately paused exclusive SMR writer has left. During the
  ** pause the real builtin above still exercises closed-gate JLOOP recovery
  ** through the immutable sidecar without waiting for body admission. */
  lua_getglobal(L, "__idle_shadow_set_next");
  lua_getglobal(L, "idle_next_wrapper");
  ljt_lua_pcall(L, 1, 0, "set ITERN invalidation iterator");
  lua_getglobal(L, "__idle_shadow_itern");
  lua_getglobal(L, "idle_shadow_input");
  ljt_lua_pcall(L, 1, 2, "reopened IDLE ITERN invalidation");
  assert(lua_tointeger(L, -2) == 5);
  assert(lua_tointeger(L, -1) == 165);
  lua_pop(L, 2);
  assert(bc_op((BCIns)la_load32_acq((const uint32_t *)itern_patch.pc)) ==
	 BC_ITERC);
  lua_getglobal(L, "__idle_shadow_set_next");
  lua_getglobal(L, "next");
  ljt_lua_pcall(L, 1, 0, "restore ITERN iterator");
  restore_patch(&loop_patch);
  restore_patch(&ret_patch);
  restore_patch(&itern_patch);

  /* Repeat the exact post-zero-sample entry race for a generic ITERL trace.
  ** The temporary denial may defer this mutator, but it must preserve every
  ** iteration and result while the foreign reclaimer makes bounded progress. */
  ctx.reclaimed = ~(uint32_t)0;
  ctx.done = 0;
  la_store32_rel(&native_hits, 0);
  gc2_jit_sweep_displaced_rel(g, 0);
  cache_base = lua_gettop(L);
  lua_getglobal(L, "__idle_reclaim_generic");
  generic_idx = lua_gettop(L);
  lua_getglobal(L, "generic_input");
  generic_input_idx = lua_gettop(L);
  assert(lua_isfunction(L, generic_idx));
  assert(lua_isnumber(L, generic_input_idx));
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&reclaimer, NULL, idle_reclaim_main, &ctx) == 0);
  wait_for_idle_reclaim_pause(&ctx);
  lj_trace_test_force_startins_retry(1);
  lua_pushvalue(L, generic_idx);
  lua_pushvalue(L, generic_input_idx);
  ljt_lua_pcall(L, 1, 2, "closed IDLE generic-loop entry");
  assert(lua_tointeger(L, -2) == 5);
  assert(lua_tointeger(L, -1) == 165);
  lua_pop(L, 2);
  assert(gc2_jit_sweep_displaced_acq(g) != 0);
  assert(la_load32_acq(&native_hits) == 0);
  assert(!lj_tg_any_jit_active(g));
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(gc2_smr_reclaiming_acq(g) != 0);
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(reclaimer, NULL) == 0);
  assert(!lj_tg_any_jit_active(g));
  assert(la_load32_acq(&ctx.done) != 0);
  assert(gc2_smr_reclaiming_acq(g) == 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  lua_settop(L, cache_base);

  lua_getglobal(L, "__idle_reclaim_probe");
  lua_pushinteger(L, 20000);
  ljt_lua_pcall(L, 1, 1, "reopened IDLE reclaim entry");
  lua_pop(L, 1);
  assert(la_load32_acq(&native_hits) != 0);

  lua_close(L);
  puts("t-jit-idle-reclaim-entry OK: IDLE reclaim owns native-entry closure");
  return 0;
}
#else
int main(void)
{
  puts("t-jit-idle-reclaim-entry SKIP: native entry fixture is x64-only");
  return 0;
}
#endif
