/*
** Native macOS ARM64 contract for the first mixed scalar BC_LOOP root.
** Integer induction controls the loop. The accumulator and its invariant
** step are doubles, so native execution exercises FPR allocation without
** relying on the separately certified KNUM/ordered-FP-guard root.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_profile.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-numeric-loop requires the admitted ARM64 root gates"
#endif

#if !LJ_HASPROFILE || !LJ_PROFILE_TGLOCAL
#error "t-arm64-jit-numeric-loop requires ARM64 TG-local profile polling"
#endif

enum {
  R_I = REF_FIRST,
  R_X,
  R_STEP,
  R_I_PRE,
  R_X_PRE,
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
  R_END
};

enum {
  NUMERIC_XPOLL_EXIT = 4,
  NUMERIC_FINAL_EXIT = 6
};

static const IRRef expected_snaprefs[] = {
  R_I, R_I_PRE, R_N, R_PRECOND, R_LOOP, R_I_BODY, R_COND
};
static const MSize expected_mapofs[] = { 0, 2, 5, 10, 14, 18, 23 };
static const uint8_t expected_nent[] = { 0, 1, 3, 2, 2, 3, 2 };
static const uint8_t expected_nslots[] = { 6, 7, 7, 6, 6, 7, 6 };
static const uint8_t expected_map_slots[] = {
  6, 3, 5, 6, 3, 5, 3, 5, 3, 5, 6, 3, 5
};
static const IRRef expected_map_refs[] = {
  R_I,
  R_X_PRE, R_I_PRE, R_I_PRE,
  R_X_PRE, R_I_PRE,
  R_X_PRE, R_I_PRE,
  R_X_PRE, R_I_PRE, R_I_PRE,
  R_X_BODY, R_I_BODY
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 numeric-loop chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static lua_Number call_mixed(lua_State *L, lua_Integer n, lua_Number x,
	lua_Number step, int integer_x, int integer_step)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, "__arm64_numeric_loop");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  if (integer_x)
    lua_pushinteger(L, (lua_Integer)x);
  else
    lua_pushnumber(L, x);
  if (integer_step)
    lua_pushinteger(L, (lua_Integer)step);
  else
    lua_pushnumber(L, step);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 numeric-loop call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static lua_Number call_one_num(lua_State *L, const char *name,
	lua_Number arg)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, arg);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 numeric negative call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static lua_Number call_negative3(lua_State *L, lua_Integer n, lua_Number x,
	lua_Number step)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, "__arm64_numeric_negative");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushnumber(L, x);
  lua_pushnumber(L, step);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 numeric negative call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

typedef enum PostAdmissionRequest {
  POSTADMISSION_PROFILE,
  POSTADMISSION_STOPREQ
} PostAdmissionRequest;

typedef struct PostAdmissionPublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  PostAdmissionRequest request;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t published;
} PostAdmissionPublisher;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void wait_for_postadmission(PostAdmissionPublisher *publisher)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      return;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 numeric root entry did not reach post-admission pause");
}

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  global_State *g = publisher->g;
  TGState *tg = publisher->tg;

  wait_for_postadmission(publisher);
  assert(gc2_hs_epoch_acq(g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  if (lj_tg_load_jit_base(tg) != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);

  if (publisher->request == POSTADMISSION_PROFILE) {
    /* This is the signal handler's final release publication. Admission has
    ** already completed, so only native XPOLL can observe it for this call. */
    lj_tg_profile_request_rel(tg, 1);
  } else {
    assert(publisher->request == POSTADMISSION_STOPREQ);
    /* One-TG form of the counted STOPREQ publication order. */
    gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(g, 1);
    gc2_hs_epoch_rel(g, publisher->epoch+1u);
    lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(tg, 1);
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  if (ir[ref].o != op || ir[ref].t.irt != type || ir[ref].op1 != op1 ||
      ir[ref].op2 != op2) {
    fprintf(stderr, "numeric IR %u got op=%u type=%u op1=%u op2=%u; "
	    "wanted op=%u type=%u op1=%u op2=%u\n",
	    (unsigned)(ref-REF_FIRST), (unsigned)ir[ref].o,
	    (unsigned)ir[ref].t.irt, (unsigned)ir[ref].op1,
	    (unsigned)ir[ref].op2, (unsigned)op, (unsigned)type,
	    (unsigned)op1, (unsigned)op2);
  }
  assert(ir[ref].o == op);
  assert(ir[ref].t.irt == type);
  assert(ir[ref].op1 == op1);
  assert(ir[ref].op2 == op2);
}

static void expect_register_class(Reg reg, IRType type)
{
  assert(ra_hasreg(reg));
  if (type == IRT_NUM) {
    assert(reg >= RID_MIN_FPR && reg < RID_MAX_FPR);
    assert(rset_test(RSET_FPR, reg));
  } else {
    assert(type == IRT_INT);
    assert(reg < RID_MAX_GPR);
    assert(rset_test(RSET_GPR, reg));
  }
}

static RegSP effective_snapshot_regsp(const GCtrace *T,
	IRRef semantic_end, SnapNo snapno, IRRef valueref)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef renref;
  RegSP rs = ir[valueref].prev;
  for (renref = trace_nins_acq(T); renref-- > semantic_end; ) {
    const IRIns *ren = &ir[renref];
    if (ren->o != IR_RENAME)
      break;
    if (ren->op1 == valueref && ren->op2 <= snapno)
      rs = ren->prev;
  }
  return rs;
}

static void expect_constants(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  assert(trace_nk_acq(T) == REF_TRUE-1u);
  assert(ir[REF_TRUE-1u].o == IR_KINT);
  assert(ir[REF_TRUE-1u].t.irt == IRT_INT);
  assert(ir[REF_TRUE-1u].i == 1);
  assert(!ra_hasspill(ir[REF_TRUE-1u].s));
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    assert(ir[ref].o == IR_KPRI);
    assert(ir[ref].t.irt == (uint8_t)(REF_NIL-ref));
    assert(ir[ref].op12 == 0);
  }
}

static void dump_unexpected_postra(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  fprintf(stderr, "numeric post-RA nins=%u, expected=%u\n",
	  (unsigned)trace_nins_acq(T), (unsigned)R_END);
  for (ref = REF_BASE; ref < trace_nins_acq(T); ref++)
    fprintf(stderr, "post-RA %d op=%u type=%u op1=%u op2=%u "
	    "r=%u s=%u prev=%u\n",
	    (int)ref-(int)REF_FIRST, (unsigned)ir[ref].o,
	    (unsigned)ir[ref].t.irt, (unsigned)ir[ref].op1,
	    (unsigned)ir[ref].op2, (unsigned)ir[ref].r,
	    (unsigned)ir[ref].s, (unsigned)ir[ref].prev);
}

static void expect_ir_shape(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  const IRRef one = REF_TRUE-1u;
  IRRef ref;

  if (trace_nins_acq(T) != R_END)
    dump_unexpected_postra(T);
  assert(trace_nins_acq(T) == R_END);
  expect_constants(T);
  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, R_I, IR_SLOAD, IRT_INT|IRT_GUARD,
	    5, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,
	    3, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,
	    4, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_I_PRE, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I, one);
  expect_ir(ir, R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI, R_STEP, R_X);
  expect_ir(ir, R_N, IR_SLOAD, IRT_INT|IRT_GUARD,
	    2, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_PRECOND, IR_GT, IRT_INT|IRT_GUARD, R_N, R_I_PRE);
  expect_ir(ir, R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, R_I_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I_PRE, one);
  expect_ir(ir, R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,
	    R_X_PRE, R_STEP);
  expect_ir(ir, R_COND, IR_LT, IRT_INT|IRT_GUARD, R_I_BODY, R_N);
  expect_ir(ir, R_I_PHI, IR_PHI, IRT_INT, R_I_PRE, R_I_BODY);
  expect_ir(ir, R_X_PHI, IR_PHI, IRT_NUM, R_X_PRE, R_X_BODY);

  expect_ir(ir, R_RENAME_I, IR_RENAME, IRT_NIL, R_I_PRE, 4);
  expect_register_class(ir[R_RENAME_I].r, IRT_INT);

  for (ref = REF_BASE; ref < trace_nins_acq(T); ref++)
    assert(!ra_hasspill(ir[ref].s));
  expect_register_class(ir[R_I].r, IRT_INT);
  expect_register_class(ir[R_X].r, IRT_NUM);
  expect_register_class(ir[R_STEP].r, IRT_NUM);
  expect_register_class(ir[R_I_PRE].r, IRT_INT);
  expect_register_class(ir[R_X_PRE].r, IRT_NUM);
  expect_register_class(ir[R_N].r, IRT_INT);
  expect_register_class(ir[R_I_BODY].r, IRT_INT);
  expect_register_class(ir[R_X_BODY].r, IRT_NUM);
  expect_register_class(ir[R_I_PHI].r, IRT_INT);
  expect_register_class(ir[R_X_PHI].r, IRT_NUM);
}

static void expect_snapshot_shape(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  const SnapShot *snap = trace_snap_acq(T);
  const SnapEntry *snapmap = trace_snapmap_acq(T);
  const IRRef semantic_end = R_X_PHI+1u;
  MSize int_values = 0, num_values = 0;
  MSize tuple = 0;
  SnapNo snapno;

  assert(trace_nsnap_acq(T) ==
	 (SnapNo)(sizeof(expected_snaprefs)/sizeof(expected_snaprefs[0])));
  assert(trace_nsnapmap_acq(T) == 27);
  for (snapno = 0; snapno < trace_nsnap_acq(T); snapno++) {
    MSize n;
    assert(snap_ref_acq(&snap[snapno]) == expected_snaprefs[snapno]);
    assert(snap_mapofs_acq(&snap[snapno]) == expected_mapofs[snapno]);
    assert(snap_nent_acq(&snap[snapno]) == expected_nent[snapno]);
    assert(snap_nslots_acq(&snap[snapno]) == expected_nslots[snapno]);
    assert(snap_topslot_acq(&snap[snapno]) == 6);
    for (n = 0; n < snap_nent_acq(&snap[snapno]); n++) {
      SnapEntry sn = snapentry_acq(
	&snapmap[snap_mapofs_acq(&snap[snapno])+n]);
      IRRef valueref = snap_ref(sn);
      IRType type;
      RegSP rs;
      assert(tuple < sizeof(expected_map_refs)/sizeof(expected_map_refs[0]));
      assert(sn == SNAP(expected_map_slots[tuple], 0,
		       expected_map_refs[tuple]));
      tuple++;
      if (irref_isk(valueref) || (sn & SNAP_FRAME))
	continue;
      assert(valueref >= REF_FIRST && valueref < semantic_end);
      type = irt_type(ir[valueref].t);
      assert(type == IRT_INT || type == IRT_NUM);
      rs = effective_snapshot_regsp(T, semantic_end, snapno, valueref);
      assert(!ra_hasspill(regsp_spill(rs)));
      expect_register_class(regsp_reg(rs), type);
      if (type == IRT_NUM)
	num_values++;
      else
	int_values++;
    }
  }
  assert(tuple == sizeof(expected_map_refs)/sizeof(expected_map_refs[0]));
  assert(int_values != 0 && num_values != 0);
}

static void expect_only_mixed_root(lua_State *L, GCproto *pt)
{
  jit_State *J = L2J(L);
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *pc;
  BCIns patched;
  TraceNo traceno;
  uint8_t admission;

  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1 && trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1 && trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
  assert(trace_spadjust_acq(T) == 0);
  admission = la_load8_acq(&T->unused1);
  assert((admission & (TRACE_ARM64_INT_LOOP_ADMITTED |
	  TRACE_ARM64_INT_FORL_ADMITTED | TRACE_ARM64_TRUE_FUNCF_ADMITTED |
	  TRACE_ARM64_INT_SIDE_ADMITTED)) == TRACE_ARM64_INT_LOOP_ADMITTED);
  assert(trace_mcode_acq(T) != NULL && trace_szmcode_acq(T) > sizeof(MCode));
  assert(trace_mcloop_acq(T) > 0 &&
	 trace_mcloop_acq(T) < trace_szmcode_acq(T));
#if LJ_ABI_BRANCH_TRACK
  assert(trace_mcode_acq(T)[0] == A64I_BTI_J);
#endif
  pc = trace_startpc_acq(T);
  assert(pc != NULL && bc_op(trace_startins_acq(T)) == BC_LOOP);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP && bc_d(patched) == 1);
  assert(proto_trace_acq(pt) == 1);
  expect_ir_shape(T);
  expect_snapshot_shape(T);
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static void expect_native_exit(ExitNo first, ExitNo last)
{
  assert(lj_trace_test_root_entry_publishes() >= 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() >= 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == first);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == last);
}

static void expect_profile_exit_and_reentry(ExitNo xpoll, ExitNo final)
{
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == xpoll);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == final);
}

static void expect_single_exit(ExitNo exitno)
{
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == exitno);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == exitno);
}

static void test_xpoll_lifecycle(lua_State *L, GCproto *pt,
	int32_t idle_vmstate)
{
  jit_State *J = L2J(L);
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtrace *T = traceref_safe(J, 1);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  void *saved_cframe = L->cframe;
  PostAdmissionPublisher publisher;
  pthread_t worker;
  int status;

  assert(snap_ref_acq(&trace_snap_acq(T)[NUMERIC_XPOLL_EXIT]) == R_LOOP);
  assert(snap_ref_acq(&trace_snap_acq(T)[NUMERIC_FINAL_EXIT]) == R_COND);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    g, tg, epoch, POSTADMISSION_PROFILE, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  assert(call_mixed(L, 20, 1.25, 0.5, 0, 0) == 11.25);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&publisher.saw_stage) == 1);
  assert(la_load32_acq(&publisher.saw_jit_base) == 1);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  /* XPOLL snapshot 4 restores slot 3 from R_X_PRE's FPR. The exact result,
  ** followed by native re-entry and exit 6, proves that restored NUM survived. */
  expect_profile_exit_and_reentry(NUMERIC_XPOLL_EXIT, NUMERIC_FINAL_EXIT);
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);
  assert(L->cframe == saved_cframe);
  expect_only_mixed_root(L, pt);

  clear_stopreq(tg);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    g, tg, epoch, POSTADMISSION_STOPREQ, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  lua_getglobal(L, "__arm64_numeric_loop");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 20);
  lua_pushnumber(L, 1.25);
  lua_pushnumber(L, 0.5);
  status = lua_pcall(L, 3, 1, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  assert(la_load32_acq(&publisher.saw_stage) == 1);
  assert(la_load32_acq(&publisher.saw_jit_base) == 1);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  expect_single_exit(NUMERIC_XPOLL_EXIT);
  assert(gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ);
  assert(gc2_hs_epoch_acq(g) == epoch+1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch+1u);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ_FRESH) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);
  assert(L->cframe == saved_cframe);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  expect_only_mixed_root(L, pt);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_mixed(L, 20, 1.25, 0.5, 0, 0) == 11.25);
  expect_single_exit(NUMERIC_FINAL_EXIT);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);
  assert(L->cframe == saved_cframe);
  expect_only_mixed_root(L, pt);
}

static void expect_no_trace(lua_State *L, const char *name)
{
  jit_State *J = L2J(L);
  GCproto *pt = global_proto(L, name);
  TraceNo traceno;
  assert(proto_trace_acq(pt) == 0);
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static void test_positive_and_hot_type_exit(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  TGState *tg;
  GCproto *pt;
  GCtrace *T;
  const BCIns *startpc;
  BCIns startins;
  int32_t idle_vmstate;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  tg = L2TG(L);
  idle_vmstate = lj_tg_vmstate_load_acq(tg);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_numeric_loop(n,x,step) local i=0 "
      "while i<n do i=i+1; x=x+step end return x end");

  assert(call_mixed(L, 20, 1.25, 0.5, 0, 0) == 11.25);
  pt = global_proto(L, "__arm64_numeric_loop");
  expect_only_mixed_root(L, pt);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);

  test_xpoll_lifecycle(L, pt, idle_vmstate);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_mixed(L, 20, 1.25, 0.5, 0, 0) == 11.25);
  expect_native_exit(6, 6);
  expect_only_mixed_root(L, pt);

  /* Integer x takes the initial NUM SLOAD exit. After one interpreted add it
  ** becomes numeric and the same root completes natively. hotexit=1 makes
  ** this a real side-recording attempt; the numeric tranche admits no side. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_mixed(L, 20, 1, 0.5, 1, 0) == 11.0);
  expect_native_exit(0, 6);
  expect_only_mixed_root(L, pt);
  T = traceref_safe(J, 1);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);

  startpc = trace_startpc_acq(T);
  startins = trace_startins_acq(T);
  run_lua(L, "jit.flush()");
  assert((BCIns)la_load32_acq((const uint32_t *)startpc) == startins);
  assert(bc_op(startins) == BC_LOOP);
  assert(proto_trace_acq(pt) == 0);
  T = traceref_safe(J, 1);
  assert(T == NULL || !trace_runnable_acq(T, 1));
  assert(lj_tg_load_jit_base(tg) == NULL);
  lua_close(L);
}

static void test_pure_num_root_isolated(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCproto *pt;
  GCtrace *T;
  const IRIns *ir;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_numeric_negative(limit) local x=0.5 "
      "while x<limit do x=x+0.5 end return x end");
  assert(call_one_num(L, "__arm64_numeric_negative", 20.25) == 20.5);
  J = L2J(L);
  pt = global_proto(L, "__arm64_numeric_negative");
  T = traceref_safe(J, 1);
  assert(proto_trace_acq(pt) == 1);
  assert(trace_runnable_acq(T, 1));
  assert(trace_root_acq(T) == 0 && trace_link_acq(T) == 1);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_topslot_acq(T) == 4 && trace_spadjust_acq(T) == 0);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert((la_load8_acq(&T->unused1) &
	  (TRACE_ARM64_INT_LOOP_ADMITTED | TRACE_ARM64_INT_FORL_ADMITTED |
	   TRACE_ARM64_TRUE_FUNCF_ADMITTED | TRACE_ARM64_INT_SIDE_ADMITTED)) ==
	 TRACE_ARM64_INT_LOOP_ADMITTED);
  assert(trace_nk_acq(T) == REF_TRUE-2u);
  ir = trace_ir_acq(T);
  assert(ir[REF_TRUE-2u].o == IR_KNUM);
  assert(ir[REF_TRUE-2u].t.irt == IRT_NUM);
  assert(ir_knum(&ir[REF_TRUE-2u])->u64 ==
	 UINT64_C(0x3fe0000000000000));
  lua_close(L);
}

static void test_conversion_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_numeric_negative(n,x,step) local i=0 "
      "while i<n do i=i+1; x=x+i end return x end");
  assert(call_negative3(L, 20, 1.25, 0.5) == 211.25);
  expect_no_trace(L, "__arm64_numeric_negative");
  lua_close(L);
}

static void test_num_mul_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_numeric_negative(n,x,step) local i=0 "
      "while i<n do i=i+1; x=x*step end return x end");
  assert(call_negative3(L, 20, 1.25, 2.0) == 1310720.0);
  expect_no_trace(L, "__arm64_numeric_negative");
  lua_close(L);
}

int main(int argc, char **argv)
{
  assert(argc == 1 || argc == 2);
  if (argc == 2)
    assert(strcmp(argv[1], "direct") == 0 ||
	   strcmp(argv[1], "randomized") == 0);
  test_positive_and_hot_type_exit();
  test_pure_num_root_isolated();
  test_conversion_rejected();
  test_num_mul_rejected();
  puts("t-arm64-jit-numeric-loop OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-numeric-loop SKIP");
  return 0;
}

#endif
