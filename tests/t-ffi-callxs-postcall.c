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
#ifndef LJ_FFI_CALLXS_TEST_ACTIVATE
#error "t-ffi-callxs-postcall requires authentic CALLXS activation"
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

static void assert_postcall_snapshot_pc(GCtrace *T, GCproto *pt)
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
      IRRef enter, scan;
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
      assert(ref + 1 < nins);
      assert(ref_is_leave(ir, ref + 1));
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
      assert(bc_op(pc[-1]) == BC_CALL || bc_op(pc[-1]) == BC_CALLM ||
	     bc_op(pc[-1]) == BC_ITERC);
      found++;
    }
  }
  assert(prefound == callxs);
  assert(found == callxs);
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
    "]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_SO')))\n"
    "_G.__callxs_postcall_lib = lib\n"
    "function _G.__callxs_postcall_run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + lib.lj_callxs_auth_once(i) end\n"
    "  return sum\n"
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
  assert_postcall_snapshot_pc(T, runpt);
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
