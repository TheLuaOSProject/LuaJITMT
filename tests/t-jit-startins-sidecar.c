/*
** Immutable prototype-side original-bytecode recovery regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !defined(LJ_GC2_TEST_HELPERS) && !defined(LJ_TRACE_TEST_HELPERS)
#error "t-jit-startins-sidecar requires GC2 or trace test helpers"
#endif

#if LJ_TARGET_X64
typedef struct PatchedPC {
  GCproto *pt;
  BCIns *pc;
  BCIns original;
} PatchedPC;

typedef struct RecordReentryCtx {
  PatchedPC victim;
  PatchedPC outer;
  BCIns victim_jloop;
  TraceNo victim_tr;
  GCtrace *victim_body;
  BCIns *saved_patchpc;
  BCIns saved_patchins;
  TraceNo traceno;
  uint8_t saved_bcskip;
  int phase;
} RecordReentryCtx;

static RecordReentryCtx record_reentry;

static void record_reentry_assert_victim_live(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  GCtrace *T;
  lj_gc2_smr_read_enter(g);
  T = traceref_safe(J, record_reentry.victim_tr);
  assert(T != NULL && T == record_reentry.victim_body);
  assert(trace_runnable_acq(T, record_reentry.victim_tr));
  assert(trace_startpc_acq(T) == record_reentry.victim.pc);
  assert(trace_startpt_acq(T) == record_reentry.victim.pt);
  assert(trace_startins_acq(T) == record_reentry.victim.original);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_szmcode_acq(T) != 0);
  lj_gc2_smr_read_leave(g);
}

static int record_reentry_cb(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TraceNo traceno = (TraceNo)lua_tointeger(L, 1);

  if (record_reentry.phase == 0 && J->pc == record_reentry.outer.pc &&
      J->root_startins_pending) {
    record_reentry.saved_patchpc = J->patchpc;
    record_reentry.saved_patchins = J->patchins;
    record_reentry.saved_bcskip = J->bcskip;
    record_reentry.traceno = traceno;
    assert(J->patchpc == NULL && J->bcskip == 0);
    assert(J->startpc == record_reentry.outer.pc);
    assert(J->cur.startins == record_reentry.outer.original);
    assert((BCIns)la_load32_acq(
      (const uint32_t *)record_reentry.victim.pc) ==
      record_reentry.victim_jloop);
    record_reentry_assert_victim_live(L);

    /* Empty input exits the already-running victim ITERN root at snapshot 0.
    ** A VM-event callback must take static startins redispatch and leave the
    ** outer recorder's token-private pending-patch fields untouched. */
    lj_trace_test_reset_exit_stats();
    lua_getglobal(L, "__sidecar_record_victim");
    ljt_lua_pcall(L, 0, 1, "sidecar RECORD callback trace exit");
    assert(lua_toboolean(L, -1));
    lua_pop(L, 1);
    assert(lj_trace_test_exit_calls() == 1);
    assert(lj_trace_test_last_exit_parent() == record_reentry.victim_tr);
    {
      global_State *g = G(L);
      GCtrace *T;
      lj_gc2_smr_read_enter(g);
      T = traceref_safe(G2J(g), record_reentry.victim_tr);
      assert(T == record_reentry.victim_body);
      assert(lj_trace_test_last_exitno() < trace_nsnap_acq(T));
      lj_gc2_smr_read_leave(g);
    }

    assert(J->root_startins_pending == 1);
    assert(J->patchpc == record_reentry.saved_patchpc);
    assert(J->patchins == record_reentry.saved_patchins);
    assert(J->bcskip == record_reentry.saved_bcskip);
    assert((BCIns)la_load32_acq(
      (const uint32_t *)record_reentry.victim.pc) ==
      record_reentry.victim_jloop);
    record_reentry_assert_victim_live(L);
    record_reentry.phase = 1;
  } else if (record_reentry.phase == 1 &&
	     traceno == record_reentry.traceno) {
    assert(J->patchpc == record_reentry.saved_patchpc);
    assert(J->patchins == record_reentry.saved_patchins);
    assert(J->bcskip == record_reentry.saved_bcskip);
    assert((BCIns)la_load32_acq(
      (const uint32_t *)record_reentry.victim.pc) ==
      record_reentry.victim_jloop);
    record_reentry_assert_victim_live(L);
    if (J->root_startins_pending) {
      /* Returning from the nested VM event may report the same pending root
      ** again before lj_record_ins() consumes its captured instruction. */
      assert(J->pc == record_reentry.outer.pc);
    } else {
      assert(J->pc != record_reentry.outer.pc);
      record_reentry.phase = 2;
    }
  }
  return 0;
}

static void record_reentry_attach(lua_State *L, int attach)
{
  lua_getglobal(L, "jit");
  lua_getfield(L, -1, "attach");
  lua_remove(L, -2);
  lua_pushcfunction(L, record_reentry_cb);
  if (attach) {
    lua_pushliteral(L, "record");
    ljt_lua_pcall(L, 2, 0, "attach RECORD reentry fixture");
  } else {
    ljt_lua_pcall(L, 1, 0, "detach RECORD reentry fixture");
  }
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

static void check_fresh_geometry(GCproto *pt)
{
  BCPos i;
  assert(pt != NULL && pt->sizebc != 0);
  assert(proto_jit_startins(pt) == proto_bc(pt) + pt->sizebc);
  assert(pt->sizept >= sizeof(GCproto) +
	 (MSize)pt->sizebc * 2u * (MSize)sizeof(BCIns));
  for (i = 0; i < pt->sizebc; i++)
    assert((BCIns)la_load32_acq(
	(const uint32_t *)&proto_jit_startins(pt)[i]) == 0);
}

static PatchedPC find_first_op(lua_State *L, const char *name, BCOp want)
{
  GCproto *pt = global_proto(L, name);
  BCIns *bc = proto_bc(pt);
  PatchedPC patch = { pt, NULL, 0 };
  BCPos i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == want) {
      patch.pc = &bc[i];
      patch.original = ins;
      return patch;
    }
  }
  assert(!"requested bytecode opcode not found");
  return patch;
}

static PatchedPC patch_first_op(lua_State *L, const char *name, BCOp want)
{
  PatchedPC patch = find_first_op(L, name, want);
  proto_jit_startins_rel(patch.pt, patch.pc, patch.original);
  bc_publish(patch.pc, BCINS_AD(BC_JLOOP, bc_a(patch.original), 1));
  assert(proto_jit_startins_acq(patch.pt, patch.pc) == patch.original);
  return patch;
}

static PatchedPC patch_itern_following_iterl(PatchedPC *itern)
{
  PatchedPC patch;
  assert(itern != NULL && itern->pt != NULL && itern->pc != NULL);
  patch.pt = itern->pt;
  patch.pc = itern->pc + 1;
  patch.original =
    (BCIns)la_load32_acq((const uint32_t *)patch.pc);
  assert(bc_op(patch.original) == BC_ITERL);
  proto_jit_startins_rel(patch.pt, patch.pc, patch.original);
  bc_publish(patch.pc, BCINS_AD(BC_JITERL, bc_a(patch.original), 0));
  return patch;
}

static PatchedPC patch_first_return(lua_State *L, const char *name)
{
  GCproto *pt = global_proto(L, name);
  BCPos i;
  for (i = 0; i < pt->sizebc; i++) {
    BCOp op = bc_op((BCIns)la_load32_acq(
      (const uint32_t *)&proto_bc(pt)[i]));
    if (op == BC_RET || op == BC_RET0 || op == BC_RET1)
      return patch_first_op(L, name, op);
  }
  assert(!"return bytecode opcode not found");
  return patch_first_op(L, name, BC_RET);
}

static void restore_patch(PatchedPC *patch)
{
  assert(patch != NULL && patch->pt != NULL && patch->pc != NULL &&
	 patch->original != 0);
  assert(proto_jit_startins_acq(patch->pt, patch->pc) == patch->original);
  bc_publish(patch->pc, patch->original);
}

static BCIns replace_op(BCIns ins, BCOp op)
{
  setbc_op(&ins, op);
  return ins;
}

static void call_itern(lua_State *L, const char *name, const char *where)
{
  lua_getglobal(L, name);
  lua_getglobal(L, "sidecar_input");
  ljt_lua_pcall(L, 1, 2, where);
  assert(lua_tointeger(L, -2) == 5);
  assert(lua_tointeger(L, -1) == 165);
  lua_pop(L, 2);
}

static void call_itern_once(lua_State *L, const char *name, const char *where)
{
  lua_getglobal(L, name);
  lua_getglobal(L, "sidecar_input");
  ljt_lua_pcall(L, 1, 2, where);
  assert(lua_tointeger(L, -2) == 1);
  assert(lua_tointeger(L, -1) == 11);
  lua_pop(L, 2);
}

static void call_itern_empty(lua_State *L, const char *name,
			     const char *where)
{
  lua_getglobal(L, name);
  ljt_lua_pcall(L, 0, 1, where);
  assert(lua_toboolean(L, -1));
  lua_pop(L, 1);
}

static void call_loop(lua_State *L, const char *name, const char *where)
{
  lua_getglobal(L, name);
  lua_pushinteger(L, 128);
  ljt_lua_pcall(L, 1, 2, where);
  assert(lua_tointeger(L, -2) == 128);
  assert(lua_tointeger(L, -1) == 8128);
  lua_pop(L, 2);
}

static void assert_no_runnable_root_at(lua_State *L, PatchedPC *patch)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TraceNo tr;
  lj_gc2_smr_read_enter(g);
  for (tr = proto_trace_acq(patch->pt); tr != 0; ) {
    GCtrace *T = traceref_safe(J, tr);
    assert(T != NULL && trace_traceno_acq(T) == tr);
    if (trace_startpc_acq(T) == patch->pc)
      assert(!trace_runnable_acq(T, tr));
    tr = trace_nextroot_acq(T);
  }
  for (tr = 1; tr < trace_sizetrace_acq(J); tr++) {
    GCtrace *T = traceref_safe(J, tr);
    if (T != NULL && trace_startpc_acq(T) == patch->pc)
      assert(!trace_runnable_acq(T, tr));
  }
  lj_gc2_smr_read_leave(g);
}

static void assert_no_live_root_at(lua_State *L, PatchedPC *patch)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TraceNo tr;
  lj_gc2_smr_read_enter(g);
  for (tr = proto_trace_acq(patch->pt); tr != 0; ) {
    GCtrace *T = traceref_safe(J, tr);
    assert(T != NULL && trace_traceno_acq(T) == tr);
    assert(trace_startpc_acq(T) != patch->pc);
    tr = trace_nextroot_acq(T);
  }
  for (tr = 1; tr < trace_sizetrace_acq(J); tr++) {
    GCtrace *T = traceref_safe(J, tr);
    if (T != NULL && trace_traceno_acq(T) == tr)
      assert(trace_startpc_acq(T) != patch->pc);
  }
  lj_gc2_smr_read_leave(g);
}

static void expect_dump_from_sidecar_with_smr_closed(lua_State *L,
						      const char *global)
{
  global_State *g = G(L);
  uint32_t expect = LJ_GC2_SMR_OPEN;
  int base = lua_gettop(L);
  int status;
  const char *dump;
  size_t len;

  lua_getglobal(L, "string");
  lua_getfield(L, -1, "dump");
  lua_remove(L, -2);
  lua_getglobal(L, global);
  assert(lua_isfunction(L, -1));
  assert(gc2_smr_readers_acq(g) == 0);
  assert(gc2_smr_reclaiming_cas(
	 g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
  status = lua_pcall(L, 1, 1, 0);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
  if (status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "closed-SMR bytecode dump failed: %s\n",
	    err ? err : "(nil)");
  }
  assert(status == LUA_OK);
  assert(gc2_smr_readers_acq(g) == 0);
  dump = lua_tolstring(L, -1, &len);
  assert(dump != NULL && len != 0);
  assert(luaL_loadbuffer(L, dump, len, "=closed-smr-sidecar") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  assert(lua_toboolean(L, -1));
  lua_settop(L, base);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  PatchedPC loop_patch, ret_patch, itern_patch, itern_once_patch;
  PatchedPC guard_patch, iterl_patch;
  PatchedPC trace_stop_patch;
  PatchedPC start_race_target, start_race_guard;
  PatchedPC post_publish_target, post_publish_guard;
  PatchedPC record_victim_target, record_outer_target;
  TraceNo post_publish_tr = 0;
  GCtrace *post_publish_body = NULL;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "function __sidecar_dump(x) return x * 5 + 2 end\n"
    "__sidecar_loaded = assert(loadstring(string.dump(__sidecar_dump)))\n"
    "function __sidecar_loop(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i end\n"
    "  return i, x\n"
    "end\n"
    "jit.off(__sidecar_loop, true)\n"
    "function __sidecar_ret(x) return x + 7 end\n"
    "jit.off(__sidecar_ret, true)\n"
    "sidecar_input = {11, 22, 33, 44, 55}\n"
    "sidecar_real_next = next\n"
    "function sidecar_next_wrapper(t, k) return sidecar_real_next(t, k) end\n"
    "jit.off(sidecar_next_wrapper, true)\n"
    "function __sidecar_itern(t)\n"
    "  local n, x = 0, 0\n"
    "  for _, v in next, t do n, x = n + 1, x + v end\n"
    "  return n, x\n"
    "end\n"
    "jit.off(__sidecar_itern, true)\n"
    "function __sidecar_itern_once(t)\n"
    "  local n, x = 0, 0\n"
    "  for _, v in next, t do n, x = n + 1, x + v; break end\n"
    "  return n, x\n"
    "end\n"
    "jit.off(__sidecar_itern_once, true)\n"
    "function __sidecar_trace_stop_loop(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i end\n"
    "  return i, x\n"
    "end\n"
    "function __sidecar_trace_start_itern(t)\n"
    "  local n, x = 0, 0\n"
    "  for _, v in next, t do n, x = n + 1, x + v end\n"
    "  return n, x\n"
    "end\n"
    "function __sidecar_post_publish_itern()\n"
    "  for _ in next, sidecar_input do end\n"
    "  return true\n"
    "end\n"
    "sidecar_record_table = sidecar_input\n"
    "sidecar_record_empty = {}\n"
    "sidecar_record_sink = false\n"
    "function __sidecar_record_victim()\n"
    "  for _ in next, sidecar_record_table do end\n"
    "  return true\n"
    "end\n"
    "function __sidecar_record_outer()\n"
    "  for _ in next, sidecar_input do sidecar_record_sink = true end\n"
    "  return true\n"
    "end\n"
    "sidecar_trace_util = require('jit.util')\n"
    "sidecar_event_starts = 0\n"
    "sidecar_event_stops = 0\n"
    "sidecar_event_aborts = 0\n"
    "sidecar_abort_trace_visible = 0\n"
    "function sidecar_count_trace_event(what, tr)\n"
    "  if what == 'start' then sidecar_event_starts = sidecar_event_starts + 1\n"
    "  elseif what == 'stop' then sidecar_event_stops = sidecar_event_stops + 1\n"
    "  elseif what == 'abort' then\n"
    "    sidecar_event_aborts = sidecar_event_aborts + 1\n"
    "    if sidecar_trace_util.traceinfo(tr) then\n"
    "      sidecar_abort_trace_visible = sidecar_abort_trace_visible + 1\n"
    "    end\n"
    "  end\n"
    "end\n"
    "sidecar_start_race_mutated = 0\n"
    "function sidecar_start_race_event(what, tr, fn)\n"
    "  sidecar_count_trace_event(what, tr)\n"
    "  if what == 'start' and fn == __sidecar_trace_start_itern and\n"
    "     sidecar_start_race_mutated == 0 then\n"
    "    sidecar_start_race_mutated = 1\n"
    "    next = sidecar_next_wrapper\n"
    "    local n, x = __sidecar_trace_start_itern(sidecar_input)\n"
    "    assert(n == 5 and x == 165)\n"
    "    next = sidecar_real_next\n"
    "  end\n"
    "end\n");

  check_fresh_geometry(global_proto(L, "__sidecar_dump"));
  check_fresh_geometry(global_proto(L, "__sidecar_loaded"));
  check_fresh_geometry(global_proto(L, "__sidecar_loop"));
  check_fresh_geometry(global_proto(L, "__sidecar_ret"));
  check_fresh_geometry(global_proto(L, "__sidecar_itern"));
  check_fresh_geometry(global_proto(L, "__sidecar_itern_once"));
  check_fresh_geometry(global_proto(L, "__sidecar_trace_stop_loop"));
  check_fresh_geometry(global_proto(L, "__sidecar_trace_start_itern"));
  check_fresh_geometry(global_proto(L, "__sidecar_post_publish_itern"));
  check_fresh_geometry(global_proto(L, "__sidecar_record_victim"));
  check_fresh_geometry(global_proto(L, "__sidecar_record_outer"));

  loop_patch = patch_first_op(L, "__sidecar_loop", BC_LOOP);
  ret_patch = patch_first_return(L, "__sidecar_ret");
  guard_patch = find_first_op(L, "__sidecar_itern", BC_ISNEXT);
  itern_patch = patch_first_op(L, "__sidecar_itern", BC_ITERN);

  lj_trace_test_force_startins_retry(1);
  lua_getglobal(L, "__sidecar_loop");
  lua_pushinteger(L, 1000);
  ljt_lua_pcall(L, 1, 2, "sidecar LOOP recovery");
  assert(lua_tointeger(L, -2) == 1000);
  assert(lua_tonumber(L, -1) == 499500.0);
  lua_pop(L, 2);

  lj_trace_test_force_startins_retry(1);
  lua_getglobal(L, "__sidecar_ret");
  lua_pushinteger(L, 35);
  ljt_lua_pcall(L, 1, 1, "sidecar RET recovery");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  lua_getglobal(L, "sidecar_next_wrapper");
  lua_setglobal(L, "next");
  lj_trace_test_force_startins_retry(1);
  lj_bc_test_force_publish_cas_collision(
    BCINS_AD(BC_JLOOP, bc_a(itern_patch.original), 0));
  call_itern(L, "__sidecar_itern", "sidecar ISNEXT/ITERN CAS recovery");
  assert((BCIns)la_load32_acq((const uint32_t *)itern_patch.pc) ==
	 replace_op(itern_patch.original, BC_ITERC));
  assert((BCIns)la_load32_acq((const uint32_t *)guard_patch.pc) ==
	 replace_op(guard_patch.original, BC_JMP));
  assert(lj_bc_test_publish_cas_collision_pending() == 0);

  lua_getglobal(L, "sidecar_real_next");
  lua_setglobal(L, "next");
  bc_publish(guard_patch.pc, guard_patch.original);
  restore_patch(&itern_patch);

  /* A stale optimized ITERN may observe a following generic JITERL after a
  ** peer despecialized and recorded the loop. The synthetic trace number zero
  ** deterministically forces static JITERL sidecar recovery. This fixture
  ** cannot pause an already-fetched ITERN while publishing ITERC, so its probe
  ** exits after the recovered first backedge; the real next trip is ITERC. */
  itern_once_patch = find_first_op(L, "__sidecar_itern_once", BC_ITERN);
  iterl_patch = patch_itern_following_iterl(&itern_once_patch);
  call_itern_once(L, "__sidecar_itern_once",
		  "sidecar stale ITERN/JITERL recovery");
  restore_patch(&iterl_patch);

  /* Exercise the opposite winner order: trace_stop saves a complete root,
  ** then loses its exact patch CAS to terminal ILOOP publication. The same
  ** protocol prevents an ITERN root from overwriting terminal ITERC. The
  ** never-enterable trace must be disconnected and retired immediately. */
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on(__sidecar_trace_stop_loop, true)\n"
    "jit.opt.start('hotloop=1', 'hotexit=100000', 'minstitch=1')\n");
  trace_stop_patch =
    find_first_op(L, "__sidecar_trace_stop_loop", BC_LOOP);
  lj_bc_test_force_publish_cas_collision(
    replace_op(trace_stop_patch.original, BC_ILOOP));
  ljt_lua_dostring(L,
    "sidecar_event_starts, sidecar_event_stops, sidecar_event_aborts = 0, 0, 0\n"
    "sidecar_abort_trace_visible = 0\n"
    "jit.attach(sidecar_count_trace_event, 'trace')\n");
  {
    int attempts;
    for (attempts = 0;
	 attempts < 32 && lj_bc_test_publish_cas_collision_pending() != 0;
	 attempts++)
      call_loop(L, "__sidecar_trace_stop_loop",
		"sidecar trace-stop CAS loss");
  }
  assert(lj_bc_test_publish_cas_collision_pending() == 0);
  ljt_lua_dostring(L,
    "jit.attach(sidecar_count_trace_event)\n"
    "assert(sidecar_event_starts >= 1)\n"
    "assert(sidecar_event_aborts >= 1)\n"
    "assert(sidecar_abort_trace_visible == sidecar_event_aborts)\n"
    "assert(sidecar_event_starts == sidecar_event_stops + sidecar_event_aborts)\n");
  assert((BCIns)la_load32_acq((const uint32_t *)trace_stop_patch.pc) ==
	 replace_op(trace_stop_patch.original, BC_ILOOP));
  assert(proto_jit_startins_acq(trace_stop_patch.pt,
				 trace_stop_patch.pc) ==
	 trace_stop_patch.original);
  assert_no_runnable_root_at(L, &trace_stop_patch);
  ljt_lua_dostring(L, "jit.flush()");
  restore_patch(&trace_stop_patch);

  /* TRACE-start callbacks run after hot-root validation while the recorder
  ** token is held. They may still execute token-independent ISNEXT failure and
  ** terminally publish ITERC. Recorder setup must consume its earlier acquired
  ** ITERN generation exactly once, then either abort or lose the final CAS
  ** without misclassifying ITERC as a parent-zero stitched trace. */
  start_race_target =
    find_first_op(L, "__sidecar_trace_start_itern", BC_ITERN);
  start_race_guard =
    find_first_op(L, "__sidecar_trace_start_itern", BC_ISNEXT);
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on(__sidecar_trace_start_itern, true)\n"
    "jit.opt.start('hotloop=1', 'hotexit=100000', 'minstitch=1')\n"
    "sidecar_event_starts, sidecar_event_stops, sidecar_event_aborts = 0, 0, 0\n"
    "sidecar_start_race_mutated = 0\n"
    "jit.attach(sidecar_start_race_event, 'trace')\n");
  {
    int attempts;
    for (attempts = 0; attempts < 32; attempts++)
      call_itern(L, "__sidecar_trace_start_itern",
		 "sidecar TRACE-start/ISNEXT race");
  }
  ljt_lua_dostring(L,
    "jit.attach(sidecar_start_race_event)\n"
    "assert(sidecar_start_race_mutated == 1)\n"
    "assert(sidecar_event_starts >= 1)\n"
    "assert(sidecar_event_starts == sidecar_event_stops + sidecar_event_aborts)\n");
  assert((BCIns)la_load32_acq((const uint32_t *)start_race_target.pc) ==
	 replace_op(start_race_target.original, BC_ITERC));
  assert((BCIns)la_load32_acq((const uint32_t *)start_race_guard.pc) ==
	 replace_op(start_race_guard.original, BC_JMP));
  assert_no_runnable_root_at(L, &start_race_target);
  ljt_lua_dostring(L, "jit.flush()");
  bc_publish(start_race_guard.pc, start_race_guard.original);
  bc_publish(start_race_target.pc, start_race_target.original);

  /* Exercise the later winner order. First publish a real ITERN root, then
  ** make ISNEXT fail. Its JLOOP -> ITERC writer must gate that exact trace
  ** before exposing the terminal generic target. */
  post_publish_target =
    find_first_op(L, "__sidecar_post_publish_itern", BC_ITERN);
  post_publish_guard =
    find_first_op(L, "__sidecar_post_publish_itern", BC_ISNEXT);
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on(__sidecar_post_publish_itern, true)\n"
    "jit.opt.start('hotloop=1', 'hotexit=100000', 'minstitch=1')\n");
  {
    int attempts;
    for (attempts = 0; attempts < 64; attempts++) {
      BCIns target = (BCIns)la_load32_acq(
	(const uint32_t *)post_publish_target.pc);
      if (bc_op(target) == BC_JLOOP)
	break;
      call_itern_empty(L, "__sidecar_post_publish_itern",
		       "sidecar publish ITERN root");
    }
  }
  {
    global_State *g = G(L);
    jit_State *J = G2J(g);
    BCIns target = (BCIns)la_load32_acq(
      (const uint32_t *)post_publish_target.pc);
    GCtrace *T;
    assert(bc_op(target) == BC_JLOOP);
    post_publish_tr = bc_d(target);
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, post_publish_tr);
    assert(trace_runnable_acq(T, post_publish_tr));
    assert(trace_startpc_acq(T) == post_publish_target.pc);
    post_publish_body = T;
    lj_gc2_smr_read_leave(g);
  }
  expect_dump_from_sidecar_with_smr_closed(
    L, "__sidecar_post_publish_itern");
  lua_getglobal(L, "sidecar_next_wrapper");
  lua_setglobal(L, "next");
  call_itern_empty(L, "__sidecar_post_publish_itern",
		   "sidecar post-publication JLOOP invalidation");
  assert((BCIns)la_load32_acq(
    (const uint32_t *)post_publish_target.pc) ==
    replace_op(post_publish_target.original, BC_ITERC));
  assert((BCIns)la_load32_acq(
    (const uint32_t *)post_publish_guard.pc) ==
    replace_op(post_publish_guard.original, BC_JMP));
  {
    global_State *g = G(L);
    jit_State *J = G2J(g);
    GCtrace *T;
    uint8_t flags;
    assert(post_publish_tr != 0 && post_publish_body != NULL);
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, post_publish_tr);
    assert(T != NULL);
    assert(T == post_publish_body);
    flags = la_load8_acq(&T->unused1);
    assert(trace_traceno_acq(T) == post_publish_tr);
    assert(la_load64_acq(&T->retire_epoch) == 0);
    assert((flags & TRACE_ENTRY_INVALIDATED) != 0);
    assert((flags & TRACE_SCOPE_FLUSH_PENDING) == 0);
    assert(!trace_runnable_acq(T, post_publish_tr));
    lj_gc2_smr_read_leave(g);
  }
  assert_no_runnable_root_at(L, &post_publish_target);
  lua_getglobal(L, "sidecar_real_next");
  lua_setglobal(L, "next");
  ljt_lua_dostring(L, "jit.flush(__sidecar_post_publish_itern, true)");
  assert_no_live_root_at(L, &post_publish_target);
  bc_publish(post_publish_guard.pc, post_publish_guard.original);
  bc_publish(post_publish_target.pc, post_publish_target.original);

  /* A RECORD callback can enter and exit existing native code while another
  ** root owns J. The nested exit must not unpatch bytecode or write the outer
  ** recorder's patchpc/patchins/bcskip, and the captured first ITERN must be
  ** consumed on its own record event. */
  record_victim_target =
    find_first_op(L, "__sidecar_record_victim", BC_ITERN);
  record_outer_target =
    find_first_op(L, "__sidecar_record_outer", BC_ITERN);
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "sidecar_record_table = sidecar_input\n"
    "jit.on(__sidecar_record_victim, true)\n"
    "jit.on(__sidecar_record_outer, true)\n"
    "jit.opt.start('hotloop=1', 'hotexit=100000', 'minstitch=1')\n");
  {
    int attempts;
    for (attempts = 0; attempts < 64; attempts++) {
      BCIns target = (BCIns)la_load32_acq(
	(const uint32_t *)record_victim_target.pc);
      if (bc_op(target) == BC_JLOOP)
	break;
      call_itern_empty(L, "__sidecar_record_victim",
		       "sidecar compile RECORD callback victim");
    }
  }
  memset(&record_reentry, 0, sizeof(record_reentry));
  record_reentry.victim = record_victim_target;
  record_reentry.outer = record_outer_target;
  record_reentry.victim_jloop = (BCIns)la_load32_acq(
    (const uint32_t *)record_victim_target.pc);
  assert(bc_op(record_reentry.victim_jloop) == BC_JLOOP);
  record_reentry.victim_tr = bc_d(record_reentry.victim_jloop);
  {
    global_State *g = G(L);
    jit_State *J = G2J(g);
    lj_gc2_smr_read_enter(g);
    record_reentry.victim_body =
      traceref_safe(J, record_reentry.victim_tr);
    assert(record_reentry.victim_body != NULL);
    assert(trace_runnable_acq(record_reentry.victim_body,
                              record_reentry.victim_tr));
    assert(trace_startpc_acq(record_reentry.victim_body) ==
           record_reentry.victim.pc);
    assert(trace_startpt_acq(record_reentry.victim_body) ==
           record_reentry.victim.pt);
    assert(trace_startins_acq(record_reentry.victim_body) ==
           record_reentry.victim.original);
    assert(trace_mcode_acq(record_reentry.victim_body) != NULL);
    assert(trace_szmcode_acq(record_reentry.victim_body) != 0);
    lj_gc2_smr_read_leave(g);
  }
  ljt_lua_dostring(L, "sidecar_record_table = sidecar_record_empty");
  record_reentry_attach(L, 1);
  {
    int attempts;
    for (attempts = 0; attempts < 32 && record_reentry.phase < 2; attempts++)
      call_itern_empty(L, "__sidecar_record_outer",
		       "sidecar outer RECORD_1ST reentry");
  }
  record_reentry_attach(L, 0);
  assert(record_reentry.phase == 2);
  assert((BCIns)la_load32_acq(
    (const uint32_t *)record_victim_target.pc) ==
    record_reentry.victim_jloop);
  {
    global_State *g = G(L);
    jit_State *J = G2J(g);
    BCIns outer_jloop = (BCIns)la_load32_acq(
      (const uint32_t *)record_outer_target.pc);
    TraceNo tr;
    GCtrace *T;
    assert(bc_op(outer_jloop) == BC_JLOOP);
    tr = bc_d(outer_jloop);
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, tr);
    assert(trace_runnable_acq(T, tr));
    assert(trace_startpc_acq(T) == record_outer_target.pc);
    assert(trace_startpt_acq(T) == record_outer_target.pt);
    assert(trace_startins_acq(T) == record_outer_target.original);
    assert(trace_mcode_acq(T) != NULL && trace_szmcode_acq(T) != 0);
    lj_gc2_smr_read_leave(g);
  }
  assert(G2J(G(L))->patchpc == NULL);
  assert(G2J(G(L))->bcskip == 0);
  assert(G2J(G(L))->root_startins_pending == 0);
  ljt_lua_dostring(L, "jit.flush()");

  restore_patch(&loop_patch);
  restore_patch(&ret_patch);
  lua_close(L);
  puts("t-jit-startins-sidecar OK: immutable recovery and exact patch CAS");
  return 0;
}
#else
int main(void)
{
  puts("t-jit-startins-sidecar SKIP: x64 recovery fixture only");
  return 0;
}
#endif
