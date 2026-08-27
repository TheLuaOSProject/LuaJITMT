/*
** Native macOS ARM64 contract for the widened spill-free scalar loop IR.
** Roots, integer constants, checked arithmetic and signed guards are the only
** admitted semantic families. Calls, allocations, sides and function entry
** remain outside this fixture.
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
#error "t-arm64-jit-scalar-loop requires the admitted-root ARM64 gate split"
#endif

#if !LJ_HASPROFILE || !LJ_PROFILE_TGLOCAL
#error "t-arm64-jit-scalar-loop requires TG-local profile polling"
#endif

typedef struct ScalarSpec {
  const char *name;
  const char *chunk;
  const IROp *ops;
  const uint8_t *snaprefs;
  MSize nops;
  SnapNo nsnap;
  int nargs;
  lua_Integer args[4];
  lua_Number expected;
} ScalarSpec;

static const IROp sub_ops[] = {
  IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_SUBOV, IR_GT, IR_LOOP, IR_XPOLL,
  IR_ADDOV, IR_SUBOV, IR_GT, IR_PHI, IR_PHI
};
static const uint8_t sub_snaps[] = { 0, 2, 3, 4, 5, 7, 8, 9 };

static const IROp mul_ops[] = {
  IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_MULOV, IR_SLOAD, IR_GT, IR_LOOP,
  IR_XPOLL, IR_ADDOV, IR_MULOV, IR_LT, IR_PHI, IR_PHI
};
static const uint8_t mul_snaps[] = { 0, 2, 3, 4, 5, 6, 8, 9, 10 };

static const IROp le_ops[] = {
  IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_ADDOV, IR_SLOAD, IR_GE, IR_LOOP,
  IR_XPOLL, IR_ADDOV, IR_ADDOV, IR_LE, IR_PHI, IR_PHI
};
static const uint8_t le_snaps[] = { 0, 2, 3, 4, 5, 6, 8, 9, 10 };

static const IROp ge_ops[] = {
  IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_SUBOV, IR_GE, IR_LOOP, IR_XPOLL,
  IR_ADDOV, IR_SUBOV, IR_GE, IR_PHI, IR_PHI
};
static const uint8_t ge_snaps[] = { 0, 2, 3, 4, 5, 7, 8, 9 };

static const IROp ne_ops[] = {
  IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_ADDOV, IR_SLOAD, IR_NE, IR_LOOP,
  IR_XPOLL, IR_ADDOV, IR_ADDOV, IR_NE, IR_PHI, IR_PHI
};
static const uint8_t ne_snaps[] = { 0, 2, 3, 4, 5, 6, 8, 9, 10 };

static const IROp eq_ops[] = {
  IR_SLOAD, IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_EQ, IR_ADDOV, IR_SLOAD,
  IR_GT, IR_LOOP, IR_XPOLL, IR_ADDOV, IR_ADDOV, IR_LT, IR_PHI, IR_PHI
};
static const uint8_t eq_snaps[] = { 0, 3, 4, 5, 6, 7, 8, 10, 11, 12 };

static const IROp runtime_arith_ops[] = {
  IR_SLOAD, IR_SLOAD, IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_SUBOV, IR_MULOV,
  IR_SLOAD, IR_GT, IR_LOOP, IR_XPOLL, IR_ADDOV, IR_SUBOV, IR_MULOV, IR_LT,
  IR_PHI, IR_PHI
};
static const uint8_t runtime_arith_snaps[] = {
  0, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14
};

static const IROp sub_underflow_ops[] = {
  IR_SLOAD, IR_SUBOV, IR_GE, IR_LOOP, IR_XPOLL, IR_SUBOV, IR_GE, IR_PHI
};
static const uint8_t sub_underflow_snaps[] = { 0, 1, 2, 3, 5, 6 };

static const ScalarSpec simple_specs[] = {
  {
    "sub",
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
    "function __arm64_scalar_loop(n) local i=n local x=0 "
    "while i>0 do x=x+i i=i-1 end return x end",
    sub_ops, sub_snaps, sizeof(sub_ops)/sizeof(sub_ops[0]),
    sizeof(sub_snaps)/sizeof(sub_snaps[0]), 1, { 20, 0 }, 210
  },
  {
    "le",
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
    "function __arm64_scalar_loop(n) local i=0 local x=0 "
    "while i<=n do x=x+i i=i+1 end return x end",
    le_ops, le_snaps, sizeof(le_ops)/sizeof(le_ops[0]),
    sizeof(le_snaps)/sizeof(le_snaps[0]), 1, { 20, 0 }, 210
  },
  {
    "ge",
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
    "function __arm64_scalar_loop(n) local i=n local x=0 "
    "while i>=1 do x=x+i i=i-1 end return x end",
    ge_ops, ge_snaps, sizeof(ge_ops)/sizeof(ge_ops[0]),
    sizeof(ge_snaps)/sizeof(ge_snaps[0]), 1, { 20, 0 }, 210
  },
  {
    "ne",
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
    "function __arm64_scalar_loop(n) local i=0 local x=0 "
    "while i~=n do i=i+1 x=x+i end return x end",
    ne_ops, ne_snaps, sizeof(ne_ops)/sizeof(ne_ops[0]),
    sizeof(ne_snaps)/sizeof(ne_snaps[0]), 1, { 20, 0 }, 210
  },
  {
    "eq",
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
    "function __arm64_scalar_loop(n,k) local i=0 local x=0 "
    "while i<n do i=i+1 if k==7 then x=x+i else x=x-i end end "
    "return x end",
    eq_ops, eq_snaps, sizeof(eq_ops)/sizeof(eq_ops[0]),
    sizeof(eq_snaps)/sizeof(eq_snaps[0]), 2, { 20, 7 }, 210
  }
};

static const ScalarSpec mul_spec = {
  "mul",
  "jit.flush(); jit.on(); "
  "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
  "function __arm64_scalar_loop(n,x) local i=0 "
  "while i<n do i=i+1 x=x*3 end return x end",
  mul_ops, mul_snaps, sizeof(mul_ops)/sizeof(mul_ops[0]),
  sizeof(mul_snaps)/sizeof(mul_snaps[0]), 2, { 10, 1 }, 59049
};

static const ScalarSpec runtime_arith_spec = {
  "runtime-arith",
  "jit.flush(); jit.on(); "
  "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
  "function __arm64_scalar_loop(n,x,s,m) local i=0 "
  "while i<n do i=i+1 x=(x-s)*m end return x end",
  runtime_arith_ops, runtime_arith_snaps,
  sizeof(runtime_arith_ops)/sizeof(runtime_arith_ops[0]),
  sizeof(runtime_arith_snaps)/sizeof(runtime_arith_snaps[0]),
  4, { 20, 10, 1, 2 }, 8388610
};

static const ScalarSpec sub_underflow_spec = {
  "sub-underflow",
  "jit.flush(); jit.on(); "
  "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
  "function __arm64_scalar_loop(i) "
  "while i>=-2147483648 do i=i-1 end return i end",
  sub_underflow_ops, sub_underflow_snaps,
  sizeof(sub_underflow_ops)/sizeof(sub_underflow_ops[0]),
  sizeof(sub_underflow_snaps)/sizeof(sub_underflow_snaps[0]),
  1, { -2147483600LL, 0 }, -2147483649.0
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 scalar chunk failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Number call_scalar(lua_State *L, const lua_Integer *args,
	int nargs)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int i, status;
  lua_getglobal(L, "__arm64_scalar_loop");
  assert(lua_isfunction(L, -1));
  for (i = 0; i < nargs; i++)
    lua_pushinteger(L, args[i]);
  status = lua_pcall(L, nargs, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 scalar call failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static void expect_no_other_traces(lua_State *L)
{
  jit_State *J = L2J(L);
  TraceNo traceno;
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static GCproto *scalar_proto(lua_State *L)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, "__arm64_scalar_loop");
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static uint8_t expected_type(const ScalarSpec *spec, MSize offset, IROp op)
{
  if (spec->ops == runtime_arith_ops &&
      (offset == 5 || offset == 12) && op == IR_SUBOV)
    return IRT_INT|IRT_GUARD;
  switch (op) {
  case IR_SLOAD:
  case IR_LT: case IR_GE: case IR_LE: case IR_GT:
  case IR_EQ: case IR_NE:
    return IRT_INT|IRT_GUARD;
  case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
    return IRT_INT|IRT_GUARD|IRT_ISPHI;
  case IR_LOOP: case IR_XPOLL:
    return IRT_NIL|IRT_GUARD;
  case IR_PHI:
    return IRT_INT;
  default:
    assert(!"unexpected scalar opcode");
    return IRT_NIL;
  }
}

static GCtrace *expect_trace(lua_State *L, const ScalarSpec *spec,
	IRRef *looprefp, SnapNo *loopsnapp)
{
  jit_State *J = L2J(L);
  GCproto *pt = scalar_proto(L);
  GCtrace *T = traceref_safe(J, 1);
  IRIns *ir;
  IRRef ref, semantic_end, loopref = 0;
  MSize nphi = 0;
  SnapNo snapno, loopsnap = (SnapNo)~0u;
  const BCIns *pc;
  BCIns patched;

  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1 && trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1 && trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
  assert(trace_spadjust_acq(T) == 0);
  assert((la_load8_acq(&T->unused1) & TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert(trace_mcode_acq(T) != NULL && trace_szmcode_acq(T) > sizeof(MCode));
  assert(trace_mcloop_acq(T) > 0 &&
	 trace_mcloop_acq(T) < trace_szmcode_acq(T));
  pc = trace_startpc_acq(T);
  assert(pc != NULL && bc_op(trace_startins_acq(T)) == BC_LOOP);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP && bc_d(patched) == 1);
  assert(proto_trace_acq(pt) == 1);

  ir = trace_ir_acq(T);
  assert(ir != NULL);
  assert(ir[REF_BASE].o == IR_BASE && ir[REF_BASE].t.irt == IRT_PGC);
  semantic_end = REF_FIRST + spec->nops;
  assert(trace_nins_acq(T) > semantic_end);
  for (ref = REF_FIRST; ref < semantic_end; ref++) {
    IROp op = spec->ops[ref-REF_FIRST];
    uint8_t type = expected_type(spec, ref-REF_FIRST, op);
    assert(ir[ref].o == op);
    if (ir[ref].t.irt != type)
      fprintf(stderr, "%s: unexpected type %u at semantic offset %u "
	      "(op %u), wanted %u\n", spec->name,
	      (unsigned)ir[ref].t.irt, (unsigned)(ref-REF_FIRST),
	      (unsigned)op, (unsigned)type);
    assert(ir[ref].t.irt == type);
    assert(!ra_hasspill(ir[ref].s));
    if (op == IR_LOOP)
      loopref = ref;
    if (op == IR_PHI)
      nphi++;
  }
  assert(loopref != 0 && ir[loopref+1].o == IR_XPOLL);
  assert(nphi > 0 && trace_nins_acq(T) == semantic_end+nphi);
  for (ref = semantic_end; ref < trace_nins_acq(T); ref++) {
    assert(ir[ref].o == IR_RENAME);
    assert(ir[ref].t.irt == IRT_NIL);
    assert(ir[ref].op1 >= REF_FIRST && ir[ref].op1 < semantic_end);
    assert(ir[ref].op2 < trace_nsnap_acq(T));
    assert(ir[ref].r < RID_MAX_GPR);
    assert(rset_test(RSET_GPR, ir[ref].r));
    assert(!ra_hasspill(ir[ref].s));
  }
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    assert(ir[ref].o == IR_KINT && ir[ref].t.irt == IRT_INT);
    assert(!ra_hasspill(ir[ref].s));
  }
  assert(trace_nsnap_acq(T) == spec->nsnap);
  for (snapno = 0; snapno < spec->nsnap; snapno++) {
    const SnapShot *snap = &trace_snap_acq(T)[snapno];
    SnapEntry *map = &trace_snapmap_acq(T)[snap_mapofs_acq(snap)];
    IRRef snapref = snap_ref_acq(snap);
    MSize n;
    assert(snapref == REF_FIRST + spec->snaprefs[snapno]);
    for (n = 0; n < snap_nent_acq(snap); n++) {
      SnapEntry sn = snapentry_acq(&map[n]);
      IRRef mapref = snap_ref(sn);
      IRRef renref;
      RegSP rs;
      if (irref_isk(mapref) || (sn & SNAP_FRAME))
	continue;
      assert(mapref >= REF_FIRST && mapref < semantic_end);
      rs = ir[mapref].prev;
      for (renref = trace_nins_acq(T); renref-- > semantic_end; ) {
	const IRIns *ren = &ir[renref];
	if (ren->o != IR_RENAME)
	  break;
	if (ren->op1 == mapref && ren->op2 <= snapno)
	  rs = ren->prev;
      }
      assert(!ra_hasspill(regsp_spill(rs)));
      assert(regsp_reg(rs) < RID_MAX_GPR);
      assert(rset_test(RSET_GPR, regsp_reg(rs)));
    }
    if (snapref == loopref)
      loopsnap = snapno;
  }
  assert(loopsnap != (SnapNo)~0u);
  expect_no_other_traces(L);
  if (looprefp) *looprefp = loopref;
  if (loopsnapp) *loopsnapp = loopsnap;
  return T;
}

static SnapNo snapshot_for_ref(const GCtrace *T, IRRef ref)
{
  SnapNo snapno;
  for (snapno = 0; snapno < trace_nsnap_acq(T); snapno++)
    if (snap_ref_acq(&trace_snap_acq(T)[snapno]) == ref)
      return snapno;
  assert(!"missing scalar snapshot");
  return 0;
}

static IRRef nth_op_after(const GCtrace *T, IRRef after, IROp op,
	unsigned nth)
{
  IRRef ref;
  IRIns *ir = trace_ir_acq(T);
  for (ref = after+1u; ref < trace_nins_acq(T); ref++) {
    if (ir[ref].o == op && --nth == 0)
      return ref;
  }
  assert(!"missing scalar operation");
  return 0;
}

static void expect_one_exit(SnapNo exitno)
{
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == exitno);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == exitno);
}

static lua_State *record_spec(const ScalarSpec *spec, GCtrace **Tp,
	IRRef *looprefp, SnapNo *loopsnapp)
{
  lua_State *L = luaL_newstate();
  int32_t idle_vmstate;
  assert(L != NULL);
  luaL_openlibs(L);
  idle_vmstate = lj_tg_vmstate_load_acq(L2TG(L));
  run_lua(L, spec->chunk);
  assert(call_scalar(L, spec->args, spec->nargs) == spec->expected);
  *Tp = expect_trace(L, spec, looprefp, loopsnapp);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(lj_tg_vmstate_load_acq(L2TG(L)) == idle_vmstate);
  return L;
}

static void test_simple_spec(const ScalarSpec *spec)
{
  GCtrace *T;
  lua_State *L = record_spec(spec, &T, NULL, NULL);
  SnapNo finalexit = trace_nsnap_acq(T)-1u;
  int32_t idle_vmstate = lj_tg_vmstate_load_acq(L2TG(L));
  static const lua_Integer false_args[2] = { 1, 6 };
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_scalar(L, spec->args, spec->nargs) == spec->expected);
  expect_one_exit(finalexit);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(lj_tg_vmstate_load_acq(L2TG(L)) == idle_vmstate);
  expect_no_other_traces(L);
  if (spec->ops == eq_ops) {
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_scalar(L, false_args, 2) == -1);
    expect_one_exit(2);
    assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
    assert(lj_tg_in_native_acq(L2TG(L)) == 0);
    assert(lj_tg_vmstate_load_acq(L2TG(L)) == idle_vmstate);
    expect_no_other_traces(L);
  }
  lua_close(L);
}

static void test_runtime_operands(void)
{
  GCtrace *T;
  IRIns *ir;
  IRRef ref, loopref;
  SnapNo loopsnap, finalexit;
  lua_State *L = record_spec(&runtime_arith_spec, &T, &loopref, &loopsnap);
  int32_t idle_vmstate = lj_tg_vmstate_load_acq(L2TG(L));

  ir = trace_ir_acq(T);
  assert(ir != NULL);
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++) {
    if (ir[ref].o == IR_SUBOV || ir[ref].o == IR_MULOV) {
      assert(ir[ref].op1 >= REF_FIRST && ir[ref].op1 < ref);
      assert(ir[ref].op2 >= REF_FIRST && ir[ref].op2 < ref);
      assert(ir[ir[ref].op1].o == IR_SLOAD ||
	     ir[ir[ref].op1].o == IR_SUBOV ||
	     ir[ir[ref].op1].o == IR_MULOV);
      assert(ir[ir[ref].op2].o == IR_SLOAD);
    }
  }
  finalexit = trace_nsnap_acq(T)-1u;
  assert(loopsnap == 6 && finalexit == 10);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_scalar(L, runtime_arith_spec.args, runtime_arith_spec.nargs) ==
	 runtime_arith_spec.expected);
  expect_one_exit(finalexit);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(lj_tg_vmstate_load_acq(L2TG(L)) == idle_vmstate);
  expect_no_other_traces(L);
  lua_close(L);
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

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(la_load32_acq(&publisher->saw_stage) == 1);
  assert(gc2_hs_epoch_acq(publisher->g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(publisher->tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(publisher->g) == 0);
  assert(lj_tg_reqmask_acq(publisher->tg) == 0);
  assert(lj_tg_poll_acq(publisher->tg) == 0);
  if (lj_tg_load_jit_base(publisher->tg) != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  if (publisher->request == POSTADMISSION_PROFILE) {
    lj_tg_profile_request_rel(publisher->tg, 1);
  } else {
    assert(publisher->request == POSTADMISSION_STOPREQ);
    gc2_hs_actions_rel(publisher->g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(publisher->g, 1);
    gc2_hs_epoch_rel(publisher->g, publisher->epoch+1u);
    lj_tg_reqmask_rel(publisher->tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(publisher->tg, 1);
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void test_mul_lifecycle(void)
{
  GCtrace *T;
  IRRef loopref, overflowref;
  SnapNo loopsnap, finalexit, overflowexit;
  lua_State *L = record_spec(&mul_spec, &T, &loopref, &loopsnap);
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  int32_t idle_vmstate = lj_tg_vmstate_load_acq(tg);
  lua_Integer overflow_args[2] = { 2, 357913941 };
  PostAdmissionPublisher publisher;
  pthread_t worker;
  int status;

  finalexit = trace_nsnap_acq(T)-1u;
  overflowref = nth_op_after(T, loopref, IR_MULOV, 1);
  overflowexit = snapshot_for_ref(T, overflowref);
  assert(loopsnap == 5 && finalexit == 8 && overflowexit == 7);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    g, tg, epoch, POSTADMISSION_PROFILE, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  assert(call_scalar(L, mul_spec.args, mul_spec.nargs) == mul_spec.expected);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == loopsnap);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == finalexit);
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);

  clear_stopreq(tg);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    g, tg, epoch, POSTADMISSION_STOPREQ, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  lua_getglobal(L, "__arm64_scalar_loop");
  lua_pushinteger(L, mul_spec.args[0]);
  lua_pushinteger(L, mul_spec.args[1]);
  status = lua_pcall(L, 2, 1, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1), "thread interrupted: VM shutdown") !=
	 NULL);
  lua_pop(L, 1);
  assert(la_load32_acq(&publisher.published) == 1);
  expect_one_exit(loopsnap);
  assert(gc2_hs_epoch_acq(g) == epoch+1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch+1u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0 && lj_tg_poll_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);
  clear_stopreq(tg);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_scalar(L, mul_spec.args, mul_spec.nargs) == mul_spec.expected);
  expect_one_exit(finalexit);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_scalar(L, overflow_args, 2) == 3221225469.0);
  expect_one_exit(overflowexit);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);
  expect_no_other_traces(L);
  lua_close(L);
}

static void test_sub_underflow(void)
{
  GCtrace *T;
  IRRef loopref, overflowref;
  SnapNo loopsnap, overflowexit;
  lua_Integer overflow_arg[1] = { -2147483647LL };
  lua_State *L = record_spec(&sub_underflow_spec, &T, &loopref, &loopsnap);
  int32_t idle_vmstate = lj_tg_vmstate_load_acq(L2TG(L));
  overflowref = nth_op_after(T, loopref, IR_SUBOV, 1);
  overflowexit = snapshot_for_ref(T, overflowref);
  assert(loopsnap == 3 && overflowexit == 4);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_scalar(L, overflow_arg, 1) == -2147483649.0);
  expect_one_exit(overflowexit);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(lj_tg_vmstate_load_acq(L2TG(L)) == idle_vmstate);
  expect_no_other_traces(L);
  lua_close(L);
}

int main(void)
{
  MSize i;
  for (i = 0; i < sizeof(simple_specs)/sizeof(simple_specs[0]); i++)
    test_simple_spec(&simple_specs[i]);
  test_runtime_operands();
  test_mul_lifecycle();
  test_sub_underflow();
  puts("t-arm64-jit-scalar-loop OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-scalar-loop SKIP");
  return 0;
}

#endif
