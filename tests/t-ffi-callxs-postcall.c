/* Authentic generated forced-exit and POSTCALL cleanup regression. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ccall.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_XSAVE_TEST_HELPERS
#error "t-ffi-callxs-postcall requires LJ_XSAVE_TEST_HELPERS"
#endif
static uint32_t finish_calls;
static uint32_t leave_hook_calls;
static uint64_t finish_old_epoch;

static void force_epoch_each(TGState *tg)
{
  uint64_t epoch;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  if (finish_calls == 0)
    finish_old_epoch = epoch;
  finish_calls++;
  assert(epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static void observe_unexpected_finish(TGState *tg)
{
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  finish_calls++;
}

static void force_fresh_stopreq_after_native_leave(TGState *tg)
{
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1);
  leave_hook_calls++;
  assert(leave_hook_calls == 1);
  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ | TGF_STOPREQ_FRESH);
  lj_ffi_native_trace_test_set_leave_hook(NULL);
}

static GCtrace *find_callxs_trace(jit_State *J)
{
  TraceNo traceno;
  for (traceno = 1; traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref(J, traceno);
    IRIns *ir;
    IRRef ref, nins;
    if (!T || trace_traceno_acq(T) != traceno)
      continue;
    ir = trace_ir_acq(T);
    nins = trace_nins_acq(T);
    for (ref = REF_FIRST; ref < nins; ref++)
      if (ir[ref].o == IR_CALLXS)
	return T;
  }
  return NULL;
}

static int proto_has_bc(GCproto *pt, BCOp wanted)
{
  BCPos pc;
  for (pc = 0; pc < pt->sizebc; pc++)
    if (bc_op(proto_bc(pt)[pc]) == wanted)
      return 1;
  return 0;
}

static void assert_exact_enter_constant(GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef ref, nins = trace_nins_acq(T);
  unsigned callxs = 0, found = 0;
  for (ref = REF_FIRST; ref < nins; ref++) {
    if (ir[ref].o == IR_CALLXS)
      callxs++;
    if (ir[ref].o == IR_CALLS &&
	ir[ref].op2 == IRCALL_lj_ffi_native_trace_enter) {
      IRIns *carg = &ir[ir[ref].op1];
      IRIns *ktrace;
      assert(carg->o == IR_CARG);
      ktrace = &ir[carg->op1];
      assert(ktrace->o == IR_KGC);
      assert(ir_kgc_acq(ktrace) == obj2gco(T));
      found++;
    }
  }
  assert(callxs != 0 && found == callxs);
}

static int ref_is_leave(IRIns *ir, IRRef ref)
{
  return ref >= REF_FIRST && ir[ref].o == IR_CALLS &&
	 ir[ref].op2 == IRCALL_lj_ffi_native_trace_leave;
}

static void assert_postcall_snapshot_pc(GCtrace *T, GCproto *pt,
					BCOp expected_callop)
{
  IRIns *ir = trace_ir_acq(T);
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  IRRef ref, nins = trace_nins_acq(T);
  MSize nsnap = trace_nsnap_acq(T), sn;
  unsigned callxs = 0, prefound = 0, found = 0;

  for (ref = REF_FIRST; ref < nins; ref++) {
    if (ir[ref].o == IR_CALLXS) {
      IRIns *preguard;
      IRRef enter, leave, scan;
      int saw_xsave = 0;
      assert(ref > REF_FIRST);
      preguard = &ir[ref - 1];
      assert(preguard->o == IR_NE && irt_isguard(preguard->t));
      enter = (preguard->op1 >= REF_FIRST &&
	       ir[preguard->op1].o == IR_CALLS &&
	       ir[preguard->op1].op2 == IRCALL_lj_ffi_native_trace_enter) ?
	      preguard->op1 : preguard->op2;
      assert(enter >= REF_FIRST && ir[enter].o == IR_CALLS &&
	     ir[enter].op2 == IRCALL_lj_ffi_native_trace_enter);
      for (scan = enter; scan > REF_FIRST; ) {
	IRIns *prev = &ir[--scan];
	if (prev->o == IR_XSAVE) {
	  for (sn = 0; sn < nsnap; sn++) {
	    if (snap_ref_acq(&snap[sn]) == scan) {
	      assert(snap_count_acq(&snap[sn]) == SNAPCOUNT_DONE);
	      prefound++;
	    }
	  }
	  saw_xsave = 1;
	  break;
	}
	assert(prev->o != IR_CALLXS);
      }
      assert(saw_xsave);
      /* Integer-width and float/u32 result normalization is deliberately pure
      ** and nonthrowing. It may insert CONV between CALLXS and native leave;
      ** no other operation may enter that post-side-effect window. */
      leave = ref + 1;
      while (leave < nins && ir[leave].o == IR_CONV) {
	assert(!irt_isguard(ir[leave].t));
	leave++;
      }
      assert(leave < nins && ref_is_leave(ir, leave));
      callxs++;
    }
  }
  assert(callxs != 0);

  for (ref = REF_FIRST; ref < nins; ref++) {
    IRIns *guard = &ir[ref];
    IRRef leave;
    if (guard->o != IR_EQ || !irt_isguard(guard->t))
      continue;
    leave = ref_is_leave(ir, guard->op1) ? guard->op1 :
	    (ref_is_leave(ir, guard->op2) ? guard->op2 : 0);
    if (!leave)
      continue;
    for (sn = 0; sn < nsnap; sn++) {
      MSize ofs, nent;
      const BCIns *pc;
#if LJ_FR2
      uint64_t pcbase;
#endif
      /* Native leave may throw on a fresh STOPREQ. Its snapshot must already
      ** be the post-call Lua caller state; the forced guard shares it. */
      if (snap_ref_acq(&snap[sn]) != leave)
	continue;
      assert(snap_count_acq(&snap[sn]) == SNAPCOUNT_DONE);
      ofs = snap_mapofs_acq(&snap[sn]);
      nent = snap_nent_acq(&snap[sn]);
      assert(ofs + nent + 1 + LJ_FR2 <= trace_nsnapmap_acq(T));
      pc = snap_pc_acq(&snapmap[ofs + nent]);
#if LJ_FR2
      memcpy(&pcbase, &snapmap[ofs + nent], sizeof(pcbase));
#endif
      assert(pc >= proto_bc(pt) && pc < proto_bc(pt) + pt->sizebc);
      assert(pc > proto_bc(pt));
#if LJ_FR2
      assert((pcbase & UINT64_C(0xff)) == 0);
#endif
      assert(snap_topslot_acq(&snap[sn]) == pt->framesize);
      assert(bc_op(pc[-1]) == expected_callop);
      found++;
    }
  }
  assert(prefound == callxs);
  assert(found == callxs);
}

static GCproto *global_lua_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  return pt;
}

static lua_Integer run_named(lua_State *L, const char *name, lua_Integer n)
{
  lua_Integer result;
  lua_getglobal(L, name);
  lua_pushinteger(L, n);
  assert(lua_pcall(L, 1, 1, 0) == 0);
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static lua_Integer run_entry_named(lua_State *L, const char *name,
				    lua_Integer n)
{
  lua_Integer result;
  lua_getglobal(L, "__callxs_postcall_compile_entry");
  lua_getglobal(L, name);
  lua_pushinteger(L, n);
  assert(lua_pcall(L, 2, 1, 0) == 0);
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static lua_Integer auth_count(lua_State *L)
{
  lua_Integer count;
  ljt_lua_loadstring(L,
    "return __callxs_postcall_lib.lj_callxs_auth_count()");
  ljt_lua_pcall(L, 0, 1, "read generated-topology foreign count");
  count = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return count;
}

static void auth_reset(lua_State *L)
{
  ljt_lua_dostring(L, "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
}

static GCtrace *exercise_forced_result(lua_State *L, jit_State *J,
				       TGState *tg, const char *run_name)
{
  GCproto *pt;
  GCtrace *T;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
  /* The compile entry supplies the physical Lua caller required while the
  ** loop root is recorded; subsequent direct invocations exercise that root. */
  assert(run_entry_named(L, run_name, 200) == 200);
  assert(auth_count(L) == 200);
  auth_reset(L);

  pt = global_lua_proto(L, run_name);
  assert(proto_has_bc(pt, BC_CALL));
  T = find_callxs_trace(J);
  if (T == NULL)
    fprintf(stderr, "%s did not produce a CALLXS trace\n", run_name);
  assert(T != NULL);
  assert_exact_enter_constant(T);
  assert_postcall_snapshot_pc(T, pt, BC_CALL);
  assert(trace_native_pins_acq(T) == 0);

  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(force_epoch_each);
  assert(run_named(L, run_name, 20) == 20);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_calls != 0);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch);
  assert(auth_count(L) == 20);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == 0);
  return T;
}

static GCtrace *exercise_generated_topology(lua_State *L, jit_State *J,
					    TGState *tg,
					    const char *run_name,
					    const char *opcode_name,
					    const char *snapshot_name,
					    BCOp expected_callop,
					    lua_Integer result_offset,
					    lua_Integer stop_effects)
{
  GCproto *opcode_pt, *snapshot_pt;
  GCtrace *T;
  lua_Integer expected;
  uint8_t old_flags;
  unsigned pass;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
  expected = 200 * 201 / 2 + result_offset * 200;
  assert(run_named(L, run_name, 200) == expected);
  assert(auth_count(L) == 200);
  auth_reset(L);

  opcode_pt = global_lua_proto(L, opcode_name);
  assert(proto_has_bc(opcode_pt, expected_callop));
  snapshot_pt = global_lua_proto(L, snapshot_name);
  T = find_callxs_trace(J);
  if (T == NULL)
    fprintf(stderr, "%s did not produce a CALLXS trace\n", run_name);
  assert(T != NULL);
  assert_exact_enter_constant(T);
  assert_postcall_snapshot_pc(T, snapshot_pt, expected_callop);
  assert(trace_native_pins_acq(T) == 0);

  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(force_epoch_each);
  expected = 20 * 21 / 2 + result_offset * 20;
  for (pass = 0; pass < 2; pass++) {
    uint32_t before = finish_calls;
    assert(run_named(L, run_name, 20) == expected);
    assert(finish_calls > before);
    assert(lj_ffi_native_frame_depth_acq(tg) == 0);
    assert(trace_native_pins_acq(T) == 0);
  }
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_calls >= 2);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch);
  assert(auth_count(L) == 40);

  auth_reset(L);
  old_flags = lj_tg_flags_acq(tg);
  leave_hook_calls = 0;
  lj_ffi_native_trace_test_set_leave_hook(
    force_fresh_stopreq_after_native_leave);
  lua_getglobal(L, run_name);
  lua_pushinteger(L, 20);
  assert(lua_pcall(L, 1, 1, 0) != 0);
  lj_ffi_native_trace_test_set_leave_hook(NULL);
  assert(leave_hook_calls == 1);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  lj_tg_flags_store_rlx(tg, old_flags);
  expected = auth_count(L);
  if (expected != stop_effects)
    fprintf(stderr, "%s STOPREQ foreign count: %lld\n", run_name,
	    (long long)expected);
  assert(expected == stop_effects);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == 0);
  return T;
}

int main(void)
{
#if !LJ_TARGET_X64
  printf("t-ffi-callxs-postcall SKIP: x64-only lowering\n");
  return 0;
#else
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace *T;
  GCproto *runpt;
  lua_Integer stop_count;
  uint8_t old_flags;
  unsigned pass;
  uint32_t old_depth = lj_ffi_native_frame_depth_acq(tg);
  uint32_t old_native = lj_tg_in_native_acq(tg);
  MSize old_slot = ccallback_slot_acq(&tg->cb);
  void *old_func = lj_tg_ffi_call_func_acq(tg);

  assert(old_depth == 0 && old_native == 0);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_auth_reset(void);\n"
    "int32_t lj_callxs_auth_count(void);\n"
    "int32_t lj_callxs_auth_once(int32_t);\n"
    "int32_t lj_callxs_auth_iter(int32_t, int32_t);\n"
    "uint32_t lj_callxs_auth_u32(uint32_t, int32_t);\n"
    "double lj_callxs_auth_vararg(int32_t, ...);\n"
    "float lj_callxs_auth_float(float, int32_t);\n"
    "int8_t lj_callxs_auth_i8(int32_t);\n"
    "uint8_t lj_callxs_auth_u8(int32_t);\n"
    "int16_t lj_callxs_auth_i16(int32_t);\n"
    "uint16_t lj_callxs_auth_u16(int32_t);\n"
    "void lj_callxs_auth_store(int32_t *, int32_t, int32_t);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_SO')))\n"
    "_G.__callxs_postcall_lib = lib\n"
    "function _G.__callxs_postcall_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_once(i) end\n"
    "  return sum\n"
    "end\n"
    "function _G.__callxs_postcall_tail_once(x)\n"
    "  return lib.lj_callxs_auth_once(x)\n"
    "end\n"
    "function _G.__callxs_postcall_tail_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + __callxs_postcall_tail_once(i) end\n"
    "  return sum\n"
    "end\n"
    "function _G.__callxs_postcall_produce_one(x) return x end\n"
    "function _G.__callxs_postcall_tail_multres(x)\n"
    "  return lib.lj_callxs_auth_once(__callxs_postcall_produce_one(x))\n"
    "end\n"
    "function _G.__callxs_postcall_tailm_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + __callxs_postcall_tail_multres(i) end\n"
    "  return sum\n"
    "end\n"
    "function _G.__callxs_postcall_callm_once(x)\n"
    "  local y = lib.lj_callxs_auth_once(__callxs_postcall_produce_one(x))\n"
    "  return y\n"
    "end\n"
    "function _G.__callxs_postcall_callm_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + __callxs_postcall_callm_once(i) end\n"
    "  return sum\n"
    "end\n"
    "function _G.__callxs_postcall_iter_run(n)\n"
    "  local sum = 0\n"
    "  for value in lib.lj_callxs_auth_iter, 0, 0 do\n"
    "    sum = sum + value\n"
    "    if value == n then break end\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "function _G.__callxs_postcall_iter_entry(n)\n"
    "  local result = __callxs_postcall_iter_run(n)\n"
    "  return result\n"
    "end\n"
    "function _G.__callxs_postcall_compile_entry(run, n)\n"
    "  local result = run(n)\n"
    "  return result\n"
    "end\n"
    "local result_p = ffi.new('int32_t[4]')\n"
    "function _G.__callxs_postcall_u32_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_u32(i, 5) end\n"
    "  assert(sum == n*0x80000000 + n*(n+1)/2 + 5*n)\n"
    "  return n\n"
    "end\n"
    "function _G.__callxs_postcall_double_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + lib.lj_callxs_auth_vararg(2, i + 0.0, 0.5)\n"
    "  end\n"
    "  assert(sum == n*(n+1)/2 + 0.5*n)\n"
    "  return n\n"
    "end\n"
    "function _G.__callxs_postcall_float_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_float(0.5, i) end\n"
    "  assert(sum == n*(n+1)/2 + 0.75*n)\n"
    "  return n\n"
    "end\n"
    "function _G.__callxs_postcall_i8_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_i8(i) end\n"
    "  assert(sum == -101*n)\n"
    "  return n\n"
    "end\n"
    "function _G.__callxs_postcall_u8_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_u8(i) end\n"
    "  assert(sum == 201*n)\n"
    "  return n\n"
    "end\n"
    "function _G.__callxs_postcall_i16_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_i16(i) end\n"
    "  assert(sum == -12345*n)\n"
    "  return n\n"
    "end\n"
    "function _G.__callxs_postcall_u16_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_u16(i) end\n"
    "  assert(sum == 54321*n)\n"
    "  return n\n"
    "end\n"
    "function _G.__callxs_postcall_void_run(n)\n"
    "  for i = 1, n do lib.lj_callxs_auth_store(result_p, i, i + 300) end\n"
    "  for i = n-3, n do assert(result_p[i % 4] == i + 300) end\n"
    "  return n\n"
    "end\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "lib.lj_callxs_auth_reset()\n"
    "assert(__callxs_postcall_run(200) == 200*201/2 + 9*200)\n"
    "assert(lib.lj_callxs_auth_count() == 200)\n"
    "lib.lj_callxs_auth_reset()\n");

  lua_getglobal(L, "__callxs_postcall_run");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  runpt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);

  T = find_callxs_trace(J);
  assert(T != NULL);
  assert_exact_enter_constant(T);
  assert_postcall_snapshot_pc(T, runpt, BC_CALL);
  assert(trace_native_pins_acq(T) == 0);

  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(force_epoch_each);
  /* hotexit=1 pressures this exact guard immediately. Repeated invocations
  ** prove it always returns through central cleanup rather than linking a side
  ** trace which would bypass the retained POSTCALL frame and pin release. */
  for (pass = 0; pass < 4; pass++) {
    uint32_t before = finish_calls;
    lua_getglobal(L, "__callxs_postcall_run");
    lua_pushinteger(L, 20);
    assert(lua_pcall(L, 1, 1, 0) == 0);
    assert(lua_tointeger(L, -1) == 20 * 21 / 2 + 9 * 20);
    lua_pop(L, 1);
    assert(finish_calls > before);
    assert(lj_ffi_native_frame_depth_acq(tg) == 0);
    assert(trace_native_pins_acq(T) == 0);
  }
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_calls >= 4);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch);

  ljt_lua_dostring(L,
    "assert(__callxs_postcall_lib.lj_callxs_auth_count() == 80)\n");

  /* The repaired root must remain re-enterable after its retained-pin exit. */
  ljt_lua_dostring(L, "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
  lua_getglobal(L, "__callxs_postcall_run");
  lua_pushinteger(L, 20);
  assert(lua_pcall(L, 1, 1, 0) == 0);
  assert(lua_tointeger(L, -1) == 20 * 21 / 2 + 9 * 20);
  lua_pop(L, 1);
  ljt_lua_dostring(L,
    "assert(__callxs_postcall_lib.lj_callxs_auth_count() == 20)\n");
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == 0);

  /* Inject a fresh STOPREQ after generated native_leave has closed native
  ** state, but before its fresh-stop decision. The leave IR and forced guard
  ** share the post-call caller snapshot, so the CCI_T unwind must neither
  ** restore before CALLXS nor retain a frame/pin. The loop root begins after
  ** one interpreted iteration, making two foreign effects the exact oracle. */
  ljt_lua_dostring(L, "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
  old_flags = lj_tg_flags_acq(tg);
  leave_hook_calls = 0;
  lj_ffi_native_trace_test_set_leave_hook(
    force_fresh_stopreq_after_native_leave);
  lua_getglobal(L, "__callxs_postcall_run");
  lua_pushinteger(L, 20);
  assert(lua_pcall(L, 1, 1, 0) != 0);
  lj_ffi_native_trace_test_set_leave_hook(NULL);
  assert(leave_hook_calls == 1);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  lj_tg_flags_store_rlx(tg, old_flags);
  ljt_lua_loadstring(L,
    "return __callxs_postcall_lib.lj_callxs_auth_count()");
  ljt_lua_pcall(L, 0, 1, "read STOPREQ foreign count");
  stop_count = lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(stop_count == 2);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == 0);

  /* A deliberately occupied native-depth slot makes generated entry reject
  ** before CALLXS. The pre-call snapshot must execute the interpreter fallback
  ** exactly once per iteration; no generated leave/POSTCALL handoff may run. */
  ljt_lua_dostring(L, "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(observe_unexpected_finish);
  lj_tg_in_native_rel(tg, 1);
  lua_getglobal(L, "__callxs_postcall_run");
  lua_pushinteger(L, 20);
  assert(lua_pcall(L, 1, 1, 0) == 0);
  assert(lua_tointeger(L, -1) == 20 * 21 / 2 + 9 * 20);
  lua_pop(L, 1);
  assert(lj_tg_in_native_acq(tg) == 1);
  lj_tg_in_native_rel(tg, 0);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_calls == 0);
  ljt_lua_dostring(L,
    "assert(__callxs_postcall_lib.lj_callxs_auth_count() == 20)\n");

  /* CALLT reuses the caller's physical Lua frame and preserves its outer CALL
  ** PC. Exercise that admitted one-adjustment topology independently, then
  ** force both epoch and throwing STOPREQ cleanup from its POSTCALL snapshot. */
  assert(trace_native_pins_acq(T) == 0);
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n"
    "assert(__callxs_postcall_tail_run(200) == 200*201/2 + 9*200)\n"
    "assert(__callxs_postcall_lib.lj_callxs_auth_count() == 200)\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n");

  lua_getglobal(L, "__callxs_postcall_tail_once");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  assert(proto_has_bc(funcproto(funcV(L->top-1)), BC_CALLT));
  lua_pop(L, 1);
  lua_getglobal(L, "__callxs_postcall_tail_run");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  runpt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);

  T = find_callxs_trace(J);
  assert(T != NULL);
  assert_exact_enter_constant(T);
  assert_postcall_snapshot_pc(T, runpt, BC_CALL);
  assert(trace_native_pins_acq(T) == 0);

  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(force_epoch_each);
  for (pass = 0; pass < 2; pass++) {
    uint32_t before = finish_calls;
    lua_getglobal(L, "__callxs_postcall_tail_run");
    lua_pushinteger(L, 20);
    assert(lua_pcall(L, 1, 1, 0) == 0);
    assert(lua_tointeger(L, -1) == 20 * 21 / 2 + 9 * 20);
    lua_pop(L, 1);
    assert(finish_calls > before);
    assert(lj_ffi_native_frame_depth_acq(tg) == 0);
    assert(trace_native_pins_acq(T) == 0);
  }
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_calls >= 2);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch);

  ljt_lua_dostring(L,
    "assert(__callxs_postcall_lib.lj_callxs_auth_count() == 40)\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
  old_flags = lj_tg_flags_acq(tg);
  leave_hook_calls = 0;
  lj_ffi_native_trace_test_set_leave_hook(
    force_fresh_stopreq_after_native_leave);
  lua_getglobal(L, "__callxs_postcall_tail_run");
  lua_pushinteger(L, 20);
  assert(lua_pcall(L, 1, 1, 0) != 0);
  lj_ffi_native_trace_test_set_leave_hook(NULL);
  assert(leave_hook_calls == 1);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  lj_tg_flags_store_rlx(tg, old_flags);
  ljt_lua_loadstring(L,
    "return __callxs_postcall_lib.lj_callxs_auth_count()");
  ljt_lua_pcall(L, 0, 1, "read tail STOPREQ foreign count");
  stop_count = lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(stop_count == 2);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == 0);

  /* CALLMT has the same reused-frame handoff, but also consumes the producer's
  ** open results. Authenticate it separately so CALLT cannot mask a recorder
  ** regression in the multiple-result tail opcode. */
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n"
    "assert(__callxs_postcall_tailm_run(200) == 200*201/2 + 9*200)\n"
    "assert(__callxs_postcall_lib.lj_callxs_auth_count() == 200)\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n");

  lua_getglobal(L, "__callxs_postcall_tail_multres");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  assert(proto_has_bc(funcproto(funcV(L->top-1)), BC_CALLMT));
  lua_pop(L, 1);
  lua_getglobal(L, "__callxs_postcall_tailm_run");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  runpt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);

  T = find_callxs_trace(J);
  assert(T != NULL);
  assert_exact_enter_constant(T);
  assert_postcall_snapshot_pc(T, runpt, BC_CALL);
  assert(trace_native_pins_acq(T) == 0);

  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(force_epoch_each);
  for (pass = 0; pass < 2; pass++) {
    uint32_t before = finish_calls;
    lua_getglobal(L, "__callxs_postcall_tailm_run");
    lua_pushinteger(L, 20);
    assert(lua_pcall(L, 1, 1, 0) == 0);
    assert(lua_tointeger(L, -1) == 20 * 21 / 2 + 9 * 20);
    lua_pop(L, 1);
    assert(finish_calls > before);
    assert(lj_ffi_native_frame_depth_acq(tg) == 0);
    assert(trace_native_pins_acq(T) == 0);
  }
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_calls >= 2);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch);

  ljt_lua_dostring(L,
    "assert(__callxs_postcall_lib.lj_callxs_auth_count() == 40)\n"
    "__callxs_postcall_lib.lj_callxs_auth_reset()\n");
  old_flags = lj_tg_flags_acq(tg);
  leave_hook_calls = 0;
  lj_ffi_native_trace_test_set_leave_hook(
    force_fresh_stopreq_after_native_leave);
  lua_getglobal(L, "__callxs_postcall_tailm_run");
  lua_pushinteger(L, 20);
  assert(lua_pcall(L, 1, 1, 0) != 0);
  lj_ffi_native_trace_test_set_leave_hook(NULL);
  assert(leave_hook_calls == 1);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  lj_tg_flags_store_rlx(tg, old_flags);
  ljt_lua_loadstring(L,
    "return __callxs_postcall_lib.lj_callxs_auth_count()");
  ljt_lua_pcall(L, 0, 1, "read tailm STOPREQ foreign count");
  stop_count = lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(stop_count == 2);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == 0);

  T = exercise_generated_topology(L, J, tg,
    "__callxs_postcall_iter_entry",
    "__callxs_postcall_iter_run",
    "__callxs_postcall_iter_run", BC_ITERC, 0, 2);
  T = exercise_generated_topology(L, J, tg,
    "__callxs_postcall_callm_run",
    "__callxs_postcall_callm_once",
    "__callxs_postcall_callm_once", BC_CALLM, 9, 1);

  /* A forced epoch exit restores each admitted scalar normalization from the
  ** post-call snapshot. Exact counters prove no completed foreign call is
  ** replayed, while each Lua runner validates its restored value or effect. */
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_u32_run");
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_double_run");
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_float_run");
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_i8_run");
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_u8_run");
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_i16_run");
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_u16_run");
  T = exercise_forced_result(L, J, tg, "__callxs_postcall_void_run");

  assert(lj_ffi_native_frame_depth_acq(tg) == old_depth);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_tg_in_native_acq(tg) == old_native);
  assert(trace_native_pins_acq(T) == 0);
  assert(ccallback_slot_acq(&tg->cb) == old_slot);
  assert(lj_tg_ffi_call_func_acq(tg) == old_func);
  assert(tg->ffi_xsave_root == NULL);
  assert(tg->ffi_xsave_baseslot == 0);
  assert(tg->ffi_xsave_nslots == 0);

  lua_close(L);
  printf("t-ffi-callxs-postcall OK: forced exits and STOPREQ unwind are exact\n");
  return 0;
#endif
}
