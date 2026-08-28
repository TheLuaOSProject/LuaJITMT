/*
** Native macOS ARM64 contract for the first pure-NUM BC_LOOP root.
**
** This intentionally certifies one exact loop: a 0.5 accumulator, an
** invariant 0.5 increment, and ordered comparisons against one dynamic NUM
** limit. Numeric conversions, other constants and operations, spills, side
** traces, calls and heap effects remain outside this tranche.
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
#error "t-arm64-jit-pure-numeric-loop requires the admitted ARM64 root gates"
#endif

#if !LJ_HASPROFILE || !LJ_PROFILE_TGLOCAL
#error "t-arm64-jit-pure-numeric-loop requires ARM64 TG-local profile polling"
#endif

enum {
  K_HALF = REF_TRUE-2u,
  R_X = REF_FIRST,
  R_X_PRE,
  R_LIMIT,
  R_PRECOND,
  R_LOOP,
  R_XPOLL,
  R_X_BODY,
  R_COND,
  R_X_PHI,
  R_NOP,
  R_END
};

enum {
  PURE_TYPE_EXIT = 1,
  PURE_PRECOND_EXIT = 2,
  PURE_XPOLL_EXIT = 3,
  PURE_FINAL_EXIT = 4
};

#define PURE_HALF_BITS UINT64_C(0x3fe0000000000000)
#define PURE_QNAN_BITS UINT64_C(0x7ff8000000000000)

static const IRRef expected_snaprefs[] = {
  R_X, R_LIMIT, R_PRECOND, R_LOOP, R_COND
};
static const MSize expected_mapofs[] = { 0, 2, 6, 9, 12 };
static const uint8_t expected_nent[] = { 0, 2, 1, 1, 1 };
static const uint8_t expected_nslots[] = { 4, 5, 4, 4, 4 };
static const uint8_t expected_pcpos[] = { 7, 3, 11, 7, 11 };
static const uint8_t expected_map_slots[] = { 3, 4, 3, 3, 3 };
static const IRRef expected_map_refs[] = {
  R_X_PRE, R_X_PRE, R_X_PRE, R_X_PRE, R_X_BODY
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 pure-numeric chunk failed: %s\n",
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

static lua_Number call_limit(lua_State *L, const char *name,
	lua_Number limit, int integer_limit)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  if (integer_limit)
    lua_pushinteger(L, (lua_Integer)limit);
  else
    lua_pushnumber(L, limit);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 pure-numeric call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static lua_Number call_two_num(lua_State *L, const char *name,
	lua_Number a, lua_Number b)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, a);
  lua_pushnumber(L, b);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 pure-numeric negative call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static lua_Number call_conversion(lua_State *L, lua_Integer n,
	lua_Number x)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, "__arm64_pure_numeric_negative");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushnumber(L, x);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 pure-numeric CONV call failed: %s\n",
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
  POSTADMISSION_STOPREQ,
  POSTADMISSION_QNAN_LIMIT
} PostAdmissionRequest;

typedef struct PostAdmissionPublisher {
  lua_State *L;
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  lua_Number expected_limit;
  PostAdmissionRequest request;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t mutated;
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
  assert(!"ARM64 pure-numeric root entry did not reach post-admission pause");
}

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  global_State *g = publisher->g;
  TGState *tg = publisher->tg;
  TValue *base;

  wait_for_postadmission(publisher);
  assert(gc2_hs_epoch_acq(g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  base = lj_tg_load_jit_base(tg);
  if (base != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  assert(base == publisher->L->base);

  if (publisher->request == POSTADMISSION_PROFILE) {
    lj_tg_profile_request_rel(tg, 1);
  } else if (publisher->request == POSTADMISSION_STOPREQ) {
    gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(g, 1);
    gc2_hs_epoch_rel(g, publisher->epoch+1u);
    lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(tg, 1);
  } else {
    TValue live;
    assert(publisher->request == POSTADMISSION_QNAN_LIMIT);
    /* SLOAD slot 2 is base[0]. The owner is paused after publishing jit_base
    ** and before entering native code, so the release store is observed by
    ** both the SLOAD and its ordered floating-point pre-guard. */
    lj_tv_load_acq(&live, &base[0]);
    assert(tvisnum(&live));
    assert(numV(&live) == publisher->expected_limit);
    tv_rawstore_rel(&base[0], PURE_QNAN_BITS);
    lj_tv_load_acq(&live, &base[0]);
    assert(tvisnum(&live) && tvisnan(&live));
    la_store32_rel(&publisher->mutated, 1);
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  if (ins.o != op || ins.t.irt != type || ins.op1 != op1 || ins.op2 != op2) {
    fprintf(stderr, "pure NUM IR %u got op=%u type=%u op1=%u op2=%u; "
	    "wanted op=%u type=%u op1=%u op2=%u\n",
	    (unsigned)(ref-REF_FIRST), (unsigned)ins.o,
	    (unsigned)ins.t.irt, (unsigned)ins.op1, (unsigned)ins.op2,
	    (unsigned)op, (unsigned)type, (unsigned)op1, (unsigned)op2);
  }
  assert(ins.o == op);
  assert(ins.t.irt == type);
  assert(ins.op1 == op1);
  assert(ins.op2 == op2);
}

static Reg expect_fpr(const IRIns *ir, IRRef ref)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  assert(ins.r >= RID_MIN_FPR && ins.r < RID_MAX_FPR);
  assert(rset_test(RSET_FPR, ins.r));
  assert(!ra_hasspill(ins.s));
  return ins.r;
}

static void dump_unexpected_postra(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  fprintf(stderr, "pure NUM post-RA nins=%u, expected=%u\n",
	  (unsigned)trace_nins_acq(T), (unsigned)R_END);
  for (ref = K_HALF; ref < trace_nins_acq(T); ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    fprintf(stderr, "post-RA %d op=%u type=%u op1=%u op2=%u "
	    "r=%u s=%u prev=%u raw=%#llx\n",
	    (int)ref-(int)REF_FIRST, (unsigned)ins.o,
	    (unsigned)ins.t.irt, (unsigned)ins.op1, (unsigned)ins.op2,
	    (unsigned)ins.r, (unsigned)ins.s, (unsigned)ins.prev,
	    (unsigned long long)ins.tv.u64);
  }
}

static void expect_ir_shape(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRIns k, payload, suffix;
  Reg xpre, xbody, xphi;
  IRRef ref;

  if (trace_nins_acq(T) != R_END)
    dump_unexpected_postra(T);
  assert(trace_nk_acq(T) == K_HALF);
  assert(trace_nins_acq(T) == R_END);

  k = ir_load_acq(&ir[K_HALF]);
  payload = ir_load_acq(&ir[K_HALF+1u]);
  assert(k.o == IR_KNUM && k.t.irt == IRT_NUM && k.op12 == 0);
  assert(k.r == RID_INIT && k.s == SPS_NONE);
  assert(payload.tv.u64 == PURE_HALF_BITS);
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns pri = ir_load_acq(&ir[ref]);
    assert(pri.o == IR_KPRI);
    assert(pri.t.irt == (uint8_t)(REF_NIL-ref));
    assert(pri.op12 == 0);
  }

  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,
	    3, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI, R_X, K_HALF);
  expect_ir(ir, R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,
	    2, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_PRECOND, IR_GT, IRT_NUM|IRT_GUARD, R_LIMIT, R_X_PRE);
  expect_ir(ir, R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,
	    R_X_PRE, K_HALF);
  expect_ir(ir, R_COND, IR_LT, IRT_NUM|IRT_GUARD, R_X_BODY, R_LIMIT);
  expect_ir(ir, R_X_PHI, IR_PHI, IRT_NUM, R_X_PRE, R_X_BODY);
  suffix = ir_load_acq(&ir[R_NOP]);
  assert(suffix.o == IR_NOP && suffix.t.irt == IRT_NIL);
  assert(suffix.op1 == 0 && suffix.op2 == 0 && suffix.prev == 0);

  for (ref = REF_BASE; ref <= R_X_PHI; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    assert(!ra_hasspill(ins.s));
    assert(ins.o != IR_RENAME);
  }
  (void)expect_fpr(ir, R_X);
  xpre = expect_fpr(ir, R_X_PRE);
  (void)expect_fpr(ir, R_LIMIT);
  xbody = expect_fpr(ir, R_X_BODY);
  xphi = expect_fpr(ir, R_X_PHI);
  assert(xpre == xbody && xpre == xphi);
}

static void expect_snapshot_shape(const GCtrace *T, const GCproto *pt)
{
  const IRIns *ir = trace_ir_acq(T);
  const SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  Reg phireg = expect_fpr(ir, R_X_PHI);
  MSize tuple = 0;
  SnapNo snapno;

  assert(trace_nsnap_acq(T) == 5);
  assert(trace_nsnapmap_acq(T) == 15);
  for (snapno = 0; snapno < 5; snapno++) {
    MSize n;
    assert(snap_ref_acq(&snap[snapno]) == expected_snaprefs[snapno]);
    assert(snap_mapofs_acq(&snap[snapno]) == expected_mapofs[snapno]);
    assert(snap_nent_acq(&snap[snapno]) == expected_nent[snapno]);
    assert(snap_nslots_acq(&snap[snapno]) == expected_nslots[snapno]);
    assert(snap_topslot_acq(&snap[snapno]) == 4);
    for (n = 0; n < expected_nent[snapno]; n++) {
      MSize mapno = expected_mapofs[snapno]+n;
      SnapEntry sn = snapentry_acq(&snapmap[mapno]);
      IRRef valueref;
      IRIns value;
      assert(tuple < sizeof(expected_map_refs)/sizeof(expected_map_refs[0]));
      assert(sn == SNAP(expected_map_slots[tuple], 0,
		       expected_map_refs[tuple]));
      valueref = snap_ref(sn);
      value = ir_load_acq(&ir[valueref]);
      assert(value.t.irt == (IRT_NUM|IRT_ISPHI));
      assert(value.r == phireg && !ra_hasspill(value.s));
      tuple++;
    }
    assert(snap_pc_acq(&snapmap[expected_mapofs[snapno]+
	   expected_nent[snapno]]) == proto_bc(pt)+expected_pcpos[snapno]);
#if LJ_FR2
    {
      uint64_t pcbase;
      SnapEntry raw[2];
      raw[0] = snapentry_acq(&snapmap[expected_mapofs[snapno]+
		expected_nent[snapno]]);
      raw[1] = snapentry_acq(&snapmap[expected_mapofs[snapno]+
		expected_nent[snapno]+1u]);
      memcpy(&pcbase, raw, sizeof(pcbase));
      assert((uint8_t)pcbase == 0);
    }
#endif
  }
  assert(tuple == sizeof(expected_map_refs)/sizeof(expected_map_refs[0]));
}

static int32_t sign_extend_branch(uint32_t value, unsigned bits)
{
  return (int32_t)(value << (32u-bits)) >> (32u-bits);
}

static void expect_ordered_fp_guard_mcode(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  const MCode *mcode = trace_mcode_acq(T);
  const MCode *exitstub = trace_exitstub_acq(T);
  MSize nword = trace_szmcode_acq(T) / sizeof(MCode);
  MSize i;
  unsigned nfcomp = 0, npre = 0, nbody = 0;
  unsigned xreg = (unsigned)(expect_fpr(ir, R_X_PHI)-RID_MIN_FPR);
  unsigned limitreg = (unsigned)(expect_fpr(ir, R_LIMIT)-RID_MIN_FPR);
  const uint32_t fcmp_mask =
    ~(uint32_t)(A64F_N(31u)|A64F_M(31u));

  assert((unsigned)(expect_fpr(ir, R_X_PRE)-RID_MIN_FPR) == xreg);
  assert((unsigned)(expect_fpr(ir, R_X_BODY)-RID_MIN_FPR) == xreg);

  assert((trace_szmcode_acq(T) & (sizeof(MCode)-1u)) == 0);
  for (i = 0; i < nword; i++) {
    uint32_t ins = mcode[i];
    if ((ins & fcmp_mask) == A64I_FCMPd) {
      uint32_t branch;
      int32_t delta;
      const MCode *target;
      const MCode *pretarget = exitstub_trace_addr_(
	(MCode *)(uintptr_t)exitstub, PURE_PRECOND_EXIT);
      const MCode *bodytarget = exitstub_trace_addr_(
	(MCode *)(uintptr_t)exitstub, PURE_FINAL_EXIT);
      const MCode *looptarget = mcode+
	trace_mcloop_acq(T)/sizeof(MCode);
      unsigned left = (ins >> 5) & 31u;
      unsigned right = (ins >> 16) & 31u;
      assert(i+1u < nword);
      branch = mcode[i+1u];
      assert((branch & UINT32_C(0xff000010)) == A64I_BCC);
      /* GT swaps its operands and LT does not, so both compare the current
      ** x register against the invariant limit register. The invariant
      ** pre-guard emits B.HS directly to exit 2. The loop-closing guard uses
      ** the assembler's inverse form: B.LO back to the loop, followed by an
      ** unconditional exit-4 branch. Thus unordered (including NaN) takes
      ** the exit path in both forms. */
      assert(left == xreg);
      assert(right == limitreg);
      delta = sign_extend_branch((branch >> 5) & 0x7ffffu, 19);
      target = &mcode[i+1u]+delta;
      if ((branch & 15u) == CC_HS) {
	assert(target == pretarget);
	npre++;
      } else {
	uint32_t exit_branch;
	int32_t exit_delta;
	assert((branch & 15u) == CC_LO);
	assert(target == looptarget);
	assert(i+2u < nword);
	exit_branch = mcode[i+2u];
	assert((exit_branch & UINT32_C(0xfc000000)) == A64I_B);
	exit_delta = sign_extend_branch(
	  exit_branch & UINT32_C(0x03ffffff), 26);
	assert(&mcode[i+2u]+exit_delta == bodytarget);
	nbody++;
      }
      nfcomp++;
    }
  }
  assert(nfcomp == 2 && npre == 1 && nbody == 1);
}

static void expect_only_pure_root(lua_State *L, GCproto *pt)
{
  jit_State *J = L2J(L);
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *pc;
  BCIns patched;
  TraceNo traceno;
  uint8_t admission;

  assert(pt->framesize == 4 && pt->sizebc == 13 && pt->numparams == 1);
  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1 && trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1 && trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_topslot_acq(T) == 4);
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
  if (pc != proto_bc(pt)+6u)
    fprintf(stderr, "pure NUM startpc offset=%td, expected=6\n",
	    pc-proto_bc(pt));
  assert(pc == proto_bc(pt)+6u);
  assert(bc_op(trace_startins_acq(T)) == BC_LOOP);
  assert(bc_a(trace_startins_acq(T)) == 2);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP && bc_d(patched) == 1);
  assert(proto_trace_acq(pt) == 1);
  expect_ir_shape(T);
  expect_snapshot_shape(T, pt);
  expect_ordered_fp_guard_mcode(T);
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static void expect_native_exit(ExitNo first, ExitNo last)
{
  if (lj_trace_test_first_exitno() != first ||
      lj_trace_test_last_exitno() != last)
    fprintf(stderr, "pure NUM exits got first=%u last=%u calls=%u "
	    "publishes=%u; wanted first=%u last=%u\n",
	    (unsigned)lj_trace_test_first_exitno(),
	    (unsigned)lj_trace_test_last_exitno(),
	    (unsigned)lj_trace_test_exit_calls(),
	    (unsigned)lj_trace_test_root_entry_publishes(),
	    (unsigned)first, (unsigned)last);
  assert(lj_trace_test_root_entry_publishes() >= 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() >= 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == first);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == last);
}

static void expect_profile_exit_and_reentry(void)
{
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == PURE_XPOLL_EXIT);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == PURE_FINAL_EXIT);
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

static void assert_native_idle(lua_State *L, int32_t idle_vmstate)
{
  TGState *tg = L2TG(L);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);
}

static void test_xpoll_lifecycle(lua_State *L, GCproto *pt,
	int32_t idle_vmstate)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtrace *T = traceref_safe(L2J(L), 1);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  void *saved_cframe = L->cframe;
  PostAdmissionPublisher publisher;
  pthread_t worker;
  int status;

  assert(snap_ref_acq(&trace_snap_acq(T)[PURE_XPOLL_EXIT]) == R_LOOP);
  assert(snap_ref_acq(&trace_snap_acq(T)[PURE_FINAL_EXIT]) == R_COND);
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
    L, g, tg, epoch, 20.25, POSTADMISSION_PROFILE, 0, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  assert(call_limit(L, "__arm64_pure_numeric_loop", 20.25, 0) == 20.5);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&publisher.saw_stage) == 1);
  assert(la_load32_acq(&publisher.saw_jit_base) == 1);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  expect_profile_exit_and_reentry();
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  expect_only_pure_root(L, pt);

  clear_stopreq(tg);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    L, g, tg, epoch, 20.25, POSTADMISSION_STOPREQ, 0, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  lua_getglobal(L, "__arm64_pure_numeric_loop");
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, 20.25);
  status = lua_pcall(L, 1, 1, 0);
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
  expect_single_exit(PURE_XPOLL_EXIT);
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
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  expect_only_pure_root(L, pt);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_limit(L, "__arm64_pure_numeric_loop", 20.25, 0) == 20.5);
  expect_single_exit(PURE_FINAL_EXIT);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  expect_only_pure_root(L, pt);
}

static void test_nan_guard_mutation(lua_State *L, GCproto *pt,
	int32_t idle_vmstate)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  PostAdmissionPublisher publisher;
  pthread_t worker;

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    L, g, tg, epoch, 20.25, POSTADMISSION_QNAN_LIMIT, 0, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  /* The owner has completed the first interpreted body iteration (x=1.0).
  ** The injected NaN must make ordered GT fail through snapshot 2 before the
  ** native body can commit another half step. */
  assert(call_limit(L, "__arm64_pure_numeric_loop", 20.25, 0) == 1.0);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&publisher.saw_stage) == 1);
  assert(la_load32_acq(&publisher.saw_jit_base) == 1);
  assert(la_load32_acq(&publisher.mutated) == 1);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  expect_single_exit(PURE_PRECOND_EXIT);
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert_native_idle(L, idle_vmstate);
  expect_only_pure_root(L, pt);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_limit(L, "__arm64_pure_numeric_loop", 20.25, 0) == 20.5);
  expect_single_exit(PURE_FINAL_EXIT);
  assert_native_idle(L, idle_vmstate);
  expect_only_pure_root(L, pt);
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

static void test_positive_and_guard_exits(void)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  GCproto *pt;
  GCtrace *T;
  const BCIns *startpc;
  BCIns startins;
  int32_t idle_vmstate;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  tg = L2TG(L);
  idle_vmstate = lj_tg_vmstate_load_acq(tg);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_pure_numeric_loop(limit) local x=0.5 "
      "while x<limit do x=x+0.5 end return x end");

  assert(call_limit(L, "__arm64_pure_numeric_loop", 20.25, 0) == 20.5);
  pt = global_proto(L, "__arm64_pure_numeric_loop");
  expect_only_pure_root(L, pt);
  assert_native_idle(L, idle_vmstate);

  test_xpoll_lifecycle(L, pt, idle_vmstate);
  test_nan_guard_mutation(L, pt, idle_vmstate);

  /* The exact non-integral NUM input can fail the speculative precondition
  ** without changing the function's interpreted result. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_limit(L, "__arm64_pure_numeric_loop", 0.75, 0) == 1.0);
  expect_native_exit(PURE_PRECOND_EXIT, PURE_PRECOND_EXIT);
  expect_only_pure_root(L, pt);

  /* An integer limit takes the NUM SLOAD type exit on every attempted native
  ** entry. hotexit=1 forces side-recording attempts, which must remain closed. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_limit(L, "__arm64_pure_numeric_loop", 20, 1) == 20.0);
  expect_native_exit(PURE_TYPE_EXIT, PURE_TYPE_EXIT);
  expect_only_pure_root(L, pt);
  T = traceref_safe(L2J(L), 1);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert_native_idle(L, idle_vmstate);

  startpc = trace_startpc_acq(T);
  startins = trace_startins_acq(T);
  run_lua(L, "jit.flush()");
  assert((BCIns)la_load32_acq((const uint32_t *)startpc) == startins);
  assert(bc_op(startins) == BC_LOOP);
  assert(proto_trace_acq(pt) == 0);
  T = traceref_safe(L2J(L), 1);
  assert(T == NULL || !trace_runnable_acq(T, 1));
  assert_native_idle(L, idle_vmstate);
  lua_close(L);
}

static void test_quarter_knum_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_pure_numeric_negative(limit) local x=0.25 "
      "while x<limit do x=x+0.25 end return x end");
  assert(call_limit(L, "__arm64_pure_numeric_negative", 20.25, 0) == 20.25);
  expect_no_trace(L, "__arm64_pure_numeric_negative");
  lua_close(L);
}

static void test_dynamic_step_separate_profile(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCproto *pt;
  GCtrace *T;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_pure_numeric_negative(limit,step) local x=0.5 "
      "while x<limit do x=x+step end return x end");
  assert(call_two_num(L, "__arm64_pure_numeric_negative",
	20.25, 0.5) == 20.5);
  J = L2J(L);
  pt = global_proto(L, "__arm64_pure_numeric_negative");
  T = traceref_safe(J, 1);
  /* Dynamic-step NUM is now a distinct exact grammar. Prove that this older
  ** fixed-half fixture sees its no-IR-constant profile, not the KNUM profile
  ** certified above. Its full native contract lives in the sibling fixture. */
  assert(pt->framesize == 5 && pt->sizebc == 14 && pt->numparams == 2);
  assert(proto_trace_acq(pt) == 1);
  assert(trace_runnable_acq(T, 1));
  assert(trace_startpt_acq(T) == pt && trace_topslot_acq(T) == 5);
  assert(trace_nk_acq(T) == REF_TRUE);
  assert(trace_nins_acq(T) == REF_BASE+12u);
  run_lua(L, "jit.flush()");
  assert(proto_trace_acq(pt) == 0);
  T = traceref_safe(J, 1);
  assert(T == NULL || !trace_runnable_acq(T, 1));
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
    "function __arm64_pure_numeric_negative(n,x) local i=0 "
      "while i<n do i=i+1; x=x+i end return x end");
  assert(call_conversion(L, 20, 1.25) == 211.25);
  expect_no_trace(L, "__arm64_pure_numeric_negative");
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
    "function __arm64_pure_numeric_negative(limit,step) local x=0.5 "
      "while x<limit do x=x*step end return x end");
  assert(call_two_num(L, "__arm64_pure_numeric_negative",
	20.25, 2.0) == 32.0);
  expect_no_trace(L, "__arm64_pure_numeric_negative");
  lua_close(L);
}

static void test_adjacent_compare_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_pure_numeric_negative(limit) local x=0.5 "
      "while x<=limit do x=x+0.5 end return x end");
  assert(call_limit(L, "__arm64_pure_numeric_negative", 20.25, 0) == 20.5);
  expect_no_trace(L, "__arm64_pure_numeric_negative");
  lua_close(L);
}

int main(int argc, char **argv)
{
  assert(argc == 1 || argc == 2);
  if (argc == 2)
    assert(strcmp(argv[1], "direct") == 0 ||
	   strcmp(argv[1], "randomized") == 0);
  test_positive_and_guard_exits();
  test_quarter_knum_rejected();
  test_dynamic_step_separate_profile();
  test_conversion_rejected();
  test_num_mul_rejected();
  test_adjacent_compare_rejected();
  puts("t-arm64-jit-pure-numeric-loop OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-pure-numeric-loop SKIP");
  return 0;
}

#endif
