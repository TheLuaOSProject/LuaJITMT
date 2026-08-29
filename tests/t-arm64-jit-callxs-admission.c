/*
** Authentic semantic and post-RA admission contract for the first Darwin
** ARM64 CALLXS roots. Published ffi.C.abs and ffi.C.fabs traces supply the
** exact immutable certificates; private copies are then mutated without
** executing them.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ctype.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT || !LJ_HASFFI || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-callxs-admission requires experimental ARM64 CALLXS"
#endif

/* Mirror the deliberately fixed private geometry in lj_asm_arm64_admit.h.
** Keeping these names local avoids turning a one-shape policy into public API. */
enum {
  CX_K_ZERO = REF_TRUE-15u,
  CX_K_ROOT = REF_TRUE-14u,
  CX_K_TRACE = REF_TRUE-13u,
  CX_K_CTYPE = REF_TRUE-11u,
  CX_K_FTSZ = REF_TRUE-10u,
  CX_K_META = REF_TRUE-8u,
  CX_K_KEY = REF_TRUE-6u,
  CX_K_TABLE = REF_TRUE-4u,
  CX_K_LIMITMAX = REF_TRUE-2u,
  CX_K_ONE = REF_TRUE-1u,

  CX_R_LIMIT = REF_FIRST,
  CX_R_LIMIT_GUARD,
  CX_R_INDEX,
  CX_R_FUNC,
  CX_R_MT,
  CX_R_MT_GUARD,
  CX_R_TABLE_ROOT,
  CX_R_KEY_ROOT,
  CX_R_LOOKUP_ARGS,
  CX_R_LOOKUP_OUT,
  CX_R_LOOKUP,
  CX_R_MOBJ,
  CX_R_MOBJ_GUARD,
  CX_R_CTYPE,
  CX_R_CTYPE_GUARD,
  CX_R_FUNCPTR,
  CX_R_XSAVE_PRE,
  CX_R_ENTER_ARGS,
  CX_R_ENTER_ROOT,
  CX_R_ENTER_PRE,
  CX_R_ENTER_GUARD_PRE,
  CX_R_CALL_PRE,
  CX_R_LEAVE_PRE,
  CX_R_LEAVE_GUARD_PRE,
  CX_R_RESULT_GUARD_PRE,
  CX_R_INDEX_PRE,
  CX_R_BOUND_GUARD_PRE,
  CX_R_LOOP,
  CX_R_XPOLL,
  CX_R_MT_BODY,
  CX_R_MT_GUARD_BODY,
  CX_R_TABLE_ROOT_BODY,
  CX_R_KEY_ROOT_BODY,
  CX_R_LOOKUP_ARGS_BODY,
  CX_R_LOOKUP_OUT_BODY,
  CX_R_LOOKUP_BODY,
  CX_R_MOBJ_BODY,
  CX_R_MOBJ_GUARD_BODY,
  CX_R_XSAVE_BODY,
  CX_R_ENTER_BODY,
  CX_R_ENTER_GUARD_BODY,
  CX_R_CALL_BODY,
  CX_R_LEAVE_BODY,
  CX_R_LEAVE_GUARD_BODY,
  CX_R_RESULT_GUARD_BODY,
  CX_R_INDEX_BODY,
  CX_R_BOUND_GUARD_BODY,
  CX_R_INDEX_PHI,
  CX_SEMANTIC_NINS,
  CX_PUBLISHED_NINS = CX_SEMANTIC_NINS+1u,

  CX_NSNAP = 15,
  CX_NSNAPMAP = 97,
  CX_BODY_RESULT_MAP = 86,
  CX_XSAVE_BODY_PC_MAP = 80,

  CX_D_R_ARG_PRE = CX_R_XSAVE_PRE,
  CX_D_R_CALL_PRE = CX_R_CALL_PRE+1u,
  CX_D_R_RESULT_GUARD_PRE = CX_R_RESULT_GUARD_PRE+1u,
  CX_D_R_ARG_BODY = CX_R_XSAVE_BODY+1u,
  CX_D_R_CALL_BODY = CX_R_CALL_BODY+2u,
  CX_D_R_RESULT_GUARD_BODY = CX_R_RESULT_GUARD_BODY+2u,
  CX_D_SEMANTIC_NINS = CX_SEMANTIC_NINS+2u,
  CX_D_PUBLISHED_NINS = CX_D_SEMANTIC_NINS+1u
};

LJ_STATIC_ASSERT(LJ_FR2 == 1);
LJ_STATIC_ASSERT(CX_SEMANTIC_NINS == REF_FIRST+48u);
LJ_STATIC_ASSERT(CX_D_R_ARG_PRE == REF_FIRST+16u);
LJ_STATIC_ASSERT(CX_D_R_CALL_PRE == REF_FIRST+22u);
LJ_STATIC_ASSERT(CX_D_R_RESULT_GUARD_PRE == REF_FIRST+25u);
LJ_STATIC_ASSERT(CX_D_R_ARG_BODY == REF_FIRST+39u);
LJ_STATIC_ASSERT(CX_D_R_CALL_BODY == REF_FIRST+43u);
LJ_STATIC_ASSERT(CX_D_R_RESULT_GUARD_BODY == REF_FIRST+46u);
LJ_STATIC_ASSERT(CX_D_SEMANTIC_NINS == REF_FIRST+50u);

typedef enum CallXSProfile {
  CX_PROFILE_I32,
  CX_PROFILE_DOUBLE
} CallXSProfile;

typedef struct CallXSFixture {
  CallXSProfile profile;
  GCtrace *owner;
  GCproto *pt;
  IRIns *ir;
  SnapShot snap[CX_NSNAP];
  SnapEntry snapmap[CX_NSNAPMAP];
  GCtrace semantic;
  CTypeID bad_ctype;
  CTypeID cross_ctype;
} CallXSFixture;

typedef struct SemanticJSave {
  GCtrace *curfinal;
  GCproto *pt;
  const BCIns *startpc;
  IRRef loopref;
  TraceNo parent;
  ExitNo exitno;
  BCReg baseslot;
  int32_t framedepth;
  int32_t retdepth;
  IRRef1 ktrace;
} SemanticJSave;

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
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

static CTypeID global_cdata_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static unsigned trace_op_count(const GCtrace *T, IROp wanted)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned count = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
    if (ir_load_acq(&ir[ref]).o == wanted)
      count++;
  return count;
}

static IRRef profile_ref(const CallXSFixture *fx, IRRef ref)
{
  if (fx->profile == CX_PROFILE_DOUBLE) {
    if (ref >= CX_R_XSAVE_BODY)
      return ref+2u;
    if (ref >= CX_R_XSAVE_PRE)
      return ref+1u;
  }
  return ref;
}

static IRRef profile_semantic_nins(const CallXSFixture *fx)
{
  return fx->profile == CX_PROFILE_DOUBLE ?
	 CX_D_SEMANTIC_NINS : CX_SEMANTIC_NINS;
}

static IRRef profile_published_nins(const CallXSFixture *fx)
{
  return fx->profile == CX_PROFILE_DOUBLE ?
	 CX_D_PUBLISHED_NINS : CX_PUBLISHED_NINS;
}

static MSize profile_spadjust(const CallXSFixture *fx)
{
  return fx->profile == CX_PROFILE_DOUBLE ? 16u : 0u;
}

static GCtrace *find_exact_callxs_trace(jit_State *J, CallXSFixture *fx)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    const IRIns *ir;
    IRIns ktrace;
    if (!trace_runnable_acq(T, traceno) || trace_root_acq(T) != 0 ||
	trace_startpt_acq(T) != fx->pt ||
	bc_op(trace_startins_acq(T)) != BC_FORL ||
	trace_startpc_acq(T) != proto_bc(fx->pt)+13 ||
	trace_nins_acq(T) != profile_published_nins(fx) ||
	trace_nk_acq(T) != CX_K_ZERO || trace_nsnap_acq(T) != CX_NSNAP ||
	trace_nsnapmap_acq(T) != CX_NSNAPMAP ||
	trace_spadjust_acq(T) != profile_spadjust(fx) ||
	trace_op_count(T, IR_XSAVE) != 2 ||
	trace_op_count(T, IR_CALLXS) != 2 ||
	trace_op_count(T, IR_CALLS) != 6 ||
	trace_op_count(T, IR_TMPREF) != 4 ||
	trace_op_count(T, IR_VLOAD) != 2 ||
	trace_op_count(T, IR_CONV) !=
	  (fx->profile == CX_PROFILE_DOUBLE ? 2u : 0u))
      continue;
    ir = trace_ir_acq(T);
    ktrace = ir_load_acq(&ir[CX_K_TRACE]);
    if (ktrace.o == IR_KGC && ktrace.t.irt == IRT_P64 &&
	ir_kgc_load_acq(&ir[CX_K_TRACE]) == obj2gco(T))
      return T;
  }
  return NULL;
}

static void assert_scalar_signature(lua_State *L, CTypeID id,
	CTypeID result)
{
  CTState *cts = ctype_ctsG(G(L));
  CType fn, arg;
  CTypeID fid;
  CTInfo info, ainfo;
  assert(cts != NULL && lj_ctype_snapshot(cts, id, &fn) > 0);
  info = ctype_info_acq(&fn);
  assert(ctype_isfunc(info) && ctype_size_acq(&fn) == 1 &&
	 (info & CTF_VARARG) == 0 && ctype_cconv(info) == CTCC_CDECL &&
	 ctype_cid(info) == result);
  fid = ctype_sib_acq(&fn);
  assert(fid != 0 && lj_ctype_snapshot(cts, fid, &arg) > 0);
  ainfo = ctype_info_acq(&arg);
  assert(ctype_isfield(ainfo) && ctype_cid(ainfo) == result &&
	 ctype_sib_acq(&arg) == 0);
}

static void assert_unsupported_signature(lua_State *L, CTypeID id)
{
  CTState *cts = ctype_ctsG(G(L));
  CType fn, arg;
  CTypeID fid;
  CTInfo info, ainfo;
  assert(cts != NULL && lj_ctype_snapshot(cts, id, &fn) > 0);
  info = ctype_info_acq(&fn);
  assert(ctype_isfunc(info) && ctype_size_acq(&fn) == 1 &&
	 (info & CTF_VARARG) == 0 && ctype_cconv(info) == CTCC_CDECL &&
	 ctype_cid(info) != CTID_INT32 && ctype_cid(info) != CTID_DOUBLE);
  fid = ctype_sib_acq(&fn);
  assert(fid != 0 && lj_ctype_snapshot(cts, fid, &arg) > 0);
  ainfo = ctype_info_acq(&arg);
  assert(ctype_isfield(ainfo) && ctype_cid(ainfo) == ctype_cid(info) &&
	 ctype_sib_acq(&arg) == 0);
}

static void reset_published_clone(CallXSFixture *fx)
{
  IRRef nk = trace_nk_acq(fx->owner);
  IRRef nins = trace_nins_acq(fx->owner);
  memset(fx->ir, 0, (size_t)nins*sizeof(IRIns));
  memcpy(&fx->ir[nk], &trace_ir_acq(fx->owner)[nk],
	 (size_t)(nins-nk)*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner), sizeof(fx->snap));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner), sizeof(fx->snapmap));
}

static void reset_semantic_clone(CallXSFixture *fx)
{
  IRIns *ktrace;
  reset_published_clone(fx);
  fx->semantic = *fx->owner;
  fx->semantic.ir = fx->ir;
  fx->semantic.snap = fx->snap;
  fx->semantic.snapmap = fx->snapmap;
  fx->semantic.nins = profile_semantic_nins(fx);

  /* Reconstruct lj_ir_ktrace() before asm_patch_ktrace() makes it a KGC. */
  ktrace = &fx->ir[CX_K_TRACE];
  ktrace[1].tv.u64 = 0;
  ktrace->op12 = 0;
  ktrace->t.irt = IRT_P64;
  ktrace->o = IR_KNUM;
  ktrace->prev = 0;
}

static SemanticJSave semantic_j_enter(jit_State *J, CallXSFixture *fx)
{
  SemanticJSave save = {
    J->curfinal, J->pt, J->startpc, J->loopref, J->parent, J->exitno,
    J->baseslot, J->framedepth, J->retdepth, J->ktrace
  };
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = proto_bc(fx->pt)+13;
  J->loopref = profile_ref(fx, CX_R_LOOP);
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1+LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = CX_K_TRACE;
  return save;
}

static void semantic_j_leave(jit_State *J, const SemanticJSave *save)
{
  J->curfinal = save->curfinal;
  J->pt = save->pt;
  J->startpc = save->startpc;
  J->loopref = save->loopref;
  J->parent = save->parent;
  J->exitno = save->exitno;
  J->baseslot = save->baseslot;
  J->framedepth = save->framedepth;
  J->retdepth = save->retdepth;
  J->ktrace = save->ktrace;
}

static void expect_semantic_admit(jit_State *J, CallXSFixture *fx)
{
  LJArm64IRReject reject;
  int admitted = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (!admitted)
    fprintf(stderr, "CALLXS semantic positive rejected: reason=%d ref=%u "
	"op=%u detail=%u\n", (int)reject.reason, (unsigned)reject.ref,
	(unsigned)reject.op, (unsigned)reject.detail);
  assert(admitted && reject.reason == LJ_ARM64_IR_REJECT_NONE);
}

static void expect_semantic_reject(jit_State *J, CallXSFixture *fx,
	LJArm64IRRejectReason reason, IRRef ref, IROp op, uint16_t detail)
{
  LJArm64IRReject reject;
  int admitted = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (admitted || reject.reason != reason || reject.ref != ref ||
	reject.op != op || reject.detail != detail)
    fprintf(stderr, "CALLXS semantic reject mismatch: admitted=%d "
	"wanted=%d/%u/%u/%u got=%d/%u/%u/%u\n", admitted, (int)reason,
	(unsigned)ref, (unsigned)op, (unsigned)detail, (int)reject.reason,
	(unsigned)reject.ref, (unsigned)reject.op, (unsigned)reject.detail);
  assert(!admitted && reject.reason == reason && reject.ref == ref &&
	 reject.op == op && reject.detail == detail);
}

static LJArm64PostRAView make_postra_view(CallXSFixture *fx)
{
  LJArm64PostRAView view = { 0 };
  reset_published_clone(fx);
  view.ir = fx->ir;
  view.owner = fx->owner;
  view.snap = fx->snap;
  view.snapmap = fx->snapmap;
  view.proto_bc = proto_bc(fx->pt);
  view.nins = trace_nins_acq(fx->owner);
  view.nk = trace_nk_acq(fx->owner);
  view.nsnap = trace_nsnap_acq(fx->owner);
  view.nsnapmap = trace_nsnapmap_acq(fx->owner);
  view.spadjust = trace_spadjust_acq(fx->owner);
  view.proto_sizebc = fx->pt->sizebc;
  view.proto_numparams = fx->pt->numparams;
  view.root_topslot = trace_topslot_acq(fx->owner);
  view.startins = trace_startins_acq(fx->owner);
  view.base_delta = 0;
  return view;
}

static void expect_postra(CallXSFixture *fx, LJArm64PostRAView *view,
	int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_postra_admit(view, &semantic_nins);
  if (result != admitted)
    fprintf(stderr, "CALLXS post-RA result mismatch: wanted=%d got=%d\n",
	admitted, result);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == profile_semantic_nins(fx));
}

static void test_double_semantic_mutations(jit_State *J,
	CallXSFixture *fx)
{
  IRRef callpre = profile_ref(fx, CX_R_CALL_PRE);
  IRRef callbody = profile_ref(fx, CX_R_CALL_BODY);
  IRRef guardpre = profile_ref(fx, CX_R_RESULT_GUARD_PRE);
  IRRef guardbody = profile_ref(fx, CX_R_RESULT_GUARD_BODY);
  IRRef indexpre = profile_ref(fx, CX_R_INDEX_PRE);

  assert(fx->profile == CX_PROFILE_DOUBLE);

  reset_semantic_clone(fx);
  assert(fx->ir[CX_D_R_ARG_PRE].o == IR_CONV &&
	 fx->ir[CX_D_R_ARG_PRE].t.irt == IRT_NUM &&
	 fx->ir[CX_D_R_ARG_PRE].op1 == CX_R_INDEX &&
	 fx->ir[CX_D_R_ARG_PRE].op2 == IRCONV_NUM_INT);
  fx->ir[CX_D_R_ARG_PRE].op2 |= IRCONV_ANY;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_D_R_ARG_PRE].t.irt = IRT_FLOAT;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_D_R_ARG_PRE].op1 = CX_R_LIMIT;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  assert(fx->ir[CX_D_R_ARG_BODY].o == IR_CONV &&
	 fx->ir[CX_D_R_ARG_BODY].t.irt == IRT_NUM &&
	 fx->ir[CX_D_R_ARG_BODY].op1 == indexpre &&
	 fx->ir[CX_D_R_ARG_BODY].op2 == IRCONV_NUM_INT);
  fx->ir[CX_D_R_ARG_BODY].op2 |= IRCONV_ANY;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_D_R_ARG_BODY].t.irt = IRT_FLOAT;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_D_R_ARG_BODY].op1 = CX_R_INDEX;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  assert(fx->ir[callpre].o == IR_CALLXS &&
	 fx->ir[callpre].t.irt == IRT_NUM &&
	 fx->ir[callpre].op1 == CX_D_R_ARG_PRE);
  fx->ir[callpre].t.irt = IRT_FLOAT;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[callpre].op1 = CX_R_INDEX;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  assert(fx->ir[callbody].o == IR_CALLXS &&
	 fx->ir[callbody].t.irt == IRT_NUM &&
	 fx->ir[callbody].op1 == CX_D_R_ARG_BODY);
  fx->ir[callbody].t.irt = IRT_FLOAT;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[callbody].op1 = indexpre;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  assert(fx->ir[guardpre].o == IR_EQ &&
	 fx->ir[guardpre].t.irt == (IRT_NUM|IRT_GUARD) &&
	 fx->ir[guardpre].op1 == callpre &&
	 fx->ir[guardpre].op2 == CX_D_R_ARG_PRE);
  fx->ir[guardpre].t.irt = IRT_INT|IRT_GUARD;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[guardpre].op1 = callbody;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[guardpre].op2 = CX_R_INDEX;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  assert(fx->ir[guardbody].o == IR_EQ &&
	 fx->ir[guardbody].t.irt == (IRT_NUM|IRT_GUARD) &&
	 fx->ir[guardbody].op1 == callbody &&
	 fx->ir[guardbody].op2 == CX_D_R_ARG_BODY);
  fx->ir[guardbody].t.irt = IRT_INT|IRT_GUARD;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[guardbody].op1 = callpre;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[guardbody].op2 = indexpre;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);
}

static void test_semantic(jit_State *J, CallXSFixture *fx)
{
  const BCIns *bc = proto_bc(fx->pt);
  BCIns saved_start = loadbc(&bc[13]);
  BCIns saved_call;
  SemanticJSave save;
  IRRef callpre = profile_ref(fx, CX_R_CALL_PRE);
  IRRef cross_callpre = fx->profile == CX_PROFILE_I32 ?
	CX_D_R_CALL_PRE : CX_R_CALL_PRE;
  IRRef loopref = profile_ref(fx, CX_R_LOOP);
  IRRef xsavepre = profile_ref(fx, CX_R_XSAVE_PRE);

  assert(bc_op(fx->owner->startins) == BC_FORL);
  bc_publish((const uint32_t *)&bc[13], fx->owner->startins);
  save = semantic_j_enter(J, fx);

  reset_semantic_clone(fx);
  expect_semantic_admit(J, fx);

  reset_semantic_clone(fx);
  ir_kgc_publish(&fx->ir[CX_K_TRACE], obj2gco(fx->owner), IRT_P64);
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, CX_K_TRACE,
	IR_KGC, 7);

  reset_semantic_clone(fx);
  fx->ir[CX_K_CTYPE].i = (int32_t)fx->bad_ctype;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, CX_K_CTYPE,
	IR_KINT, 8);

  reset_semantic_clone(fx);
  fx->ir[CX_K_CTYPE].i = (int32_t)fx->cross_ctype;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, cross_callpre,
	IR_CALLXS, 2);

  if (fx->profile == CX_PROFILE_I32) {
    reset_semantic_clone(fx);
    saved_call = loadbc(&bc[7]);
    assert(bc_op(saved_call) == BC_CALL && bc_b(saved_call) == 2);
    bc_publish((const uint32_t *)&bc[7],
	BCINS_ABC(BC_CALL, bc_a(saved_call), 3, bc_c(saved_call)));
    expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, loopref,
	IR_LOOP, 9);
    bc_publish((const uint32_t *)&bc[7], saved_call);
  }

  reset_semantic_clone(fx);
  fx->ir[profile_ref(fx, CX_R_XSAVE_BODY)].o = IR_NOP;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_R_MT].op2++;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_R_MT_GUARD].op2 = CX_K_META;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_R_KEY_ROOT].op2 = IRTMPREF_IN1;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_R_LOOKUP].op2 = IRCALL_lj_tab_gettv_rooted+1u;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_R_MOBJ].t.irt = IRT_TAB|IRT_GUARD;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[CX_R_MOBJ_GUARD].op2 = CX_K_TABLE;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[profile_ref(fx, CX_R_KEY_ROOT_BODY)].op2 = IRTMPREF_IN1;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, callpre,
	IR_CALLXS, 10);

  if (fx->profile == CX_PROFILE_DOUBLE)
    test_double_semantic_mutations(J, fx);

  reset_semantic_clone(fx);
  assert((uint8_t)fx->snapmap[16] == 0);
  ((uint8_t *)&fx->snapmap[16])[0] = 1;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_SNAPSHOT,
	xsavepre, IR_XSAVE, 11);

  reset_semantic_clone(fx);
  assert((uint8_t)fx->snapmap[26] == 8);
  ((uint8_t *)&fx->snapmap[26])[0] = 9;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_SNAPSHOT,
	xsavepre, IR_XSAVE, 11);

  reset_semantic_clone(fx);
  assert((uint8_t)fx->snapmap[CX_XSAVE_BODY_PC_MAP] == 8);
  ((uint8_t *)&fx->snapmap[CX_XSAVE_BODY_PC_MAP])[0] = 9;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_SNAPSHOT,
	xsavepre, IR_XSAVE, 11);

  semantic_j_leave(J, &save);
  bc_publish((const uint32_t *)&bc[13], saved_start);
}

static void test_postra(CallXSFixture *fx)
{
  const BCIns *bc = proto_bc(fx->pt);
  LJArm64PostRAView view;
  GCtrace decoy = *fx->owner;
  BCIns saved_start;
  TraceNo wrong_traceno;
  IRRef callpre = profile_ref(fx, CX_R_CALL_PRE);
  IRRef callbody = profile_ref(fx, CX_R_CALL_BODY);
  IRRef indexpre = profile_ref(fx, CX_R_INDEX_PRE);
  IRRef xsavebody = profile_ref(fx, CX_R_XSAVE_BODY);
  IRRef semantic_nins = profile_semantic_nins(fx);

  view = make_postra_view(fx);
  assert(view.nins == profile_published_nins(fx) &&
	 view.nsnap == CX_NSNAP && view.nsnapmap == CX_NSNAPMAP &&
	 view.spadjust == profile_spadjust(fx));
  expect_postra(fx, &view, 1);

  view = make_postra_view(fx);
  decoy.traceno = (TraceNo1)(trace_traceno_acq(fx->owner)+1u);
  view.owner = &decoy;
  expect_postra(fx, &view, 0);

  view = make_postra_view(fx);
  saved_start = loadbc(&bc[13]);
  wrong_traceno = trace_traceno_acq(fx->owner)+1u;
  assert(wrong_traceno != trace_traceno_acq(fx->owner) &&
	 wrong_traceno <= BCMAX_D);
  bc_publish((const uint32_t *)&bc[13],
	BCINS_AD(BC_JFORL, bc_a(view.startins), wrong_traceno));
  expect_postra(fx, &view, 0);
  bc_publish((const uint32_t *)&bc[13], saved_start);

  view = make_postra_view(fx);
  fx->ir[xsavebody].o = IR_NOP;
  expect_postra(fx, &view, 0);

  view = make_postra_view(fx);
  assert(fx->snapmap[CX_BODY_RESULT_MAP] ==
	 SNAP(8, 0, callbody));
  fx->snapmap[CX_BODY_RESULT_MAP] = SNAP(8, 0, indexpre);
  expect_postra(fx, &view, 0);

  view = make_postra_view(fx);
  assert(fx->ir[CX_R_FUNCPTR].r == RID_X20);
  fx->ir[CX_R_FUNCPTR].r = RID_X0;
  expect_postra(fx, &view, 0);

  if (fx->profile == CX_PROFILE_I32) {
    view = make_postra_view(fx);
    assert(fx->ir[callpre].s == 3 && fx->ir[callbody].s == 2);
    fx->ir[callpre].s = 2;
    fx->ir[callbody].s = 3;
    expect_postra(fx, &view, 0);
  } else {
    view = make_postra_view(fx);
    assert(fx->ir[CX_D_R_ARG_PRE].r == RID_D9 &&
	   fx->ir[CX_D_R_ARG_PRE].s == SPS_NONE &&
	   fx->ir[CX_D_R_ARG_BODY].r == RID_D15 &&
	   fx->ir[CX_D_R_ARG_BODY].s == SPS_NONE &&
	   fx->ir[callpre].r == RID_D0 && fx->ir[callpre].s == 4 &&
	   fx->ir[callbody].r == RID_D0 && fx->ir[callbody].s == 2);
    fx->ir[CX_D_R_ARG_PRE].r = RID_D10;
    expect_postra(fx, &view, 0);

    view = make_postra_view(fx);
    fx->ir[CX_D_R_ARG_BODY].r = RID_D9;
    expect_postra(fx, &view, 0);

    view = make_postra_view(fx);
    fx->ir[CX_D_R_ARG_PRE].s = 2;
    expect_postra(fx, &view, 0);

    view = make_postra_view(fx);
    fx->ir[callpre].r = RID_D1;
    expect_postra(fx, &view, 0);

    view = make_postra_view(fx);
    fx->ir[callbody].r = RID_D1;
    expect_postra(fx, &view, 0);

    view = make_postra_view(fx);
    fx->ir[callpre].s = 2;
    expect_postra(fx, &view, 0);

    view = make_postra_view(fx);
    fx->ir[callbody].s = 4;
    expect_postra(fx, &view, 0);

    view = make_postra_view(fx);
    view.spadjust = 0;
    expect_postra(fx, &view, 0);
  }

  view = make_postra_view(fx);
  fx->ir[semantic_nins].op1 = profile_ref(fx, CX_R_INDEX_PHI);
  fx->ir[semantic_nins].op2 = 0;
  fx->ir[semantic_nins].t.irt = IRT_NIL;
  fx->ir[semantic_nins].o = IR_RENAME;
  fx->ir[semantic_nins].prev = 0;
  expect_postra(fx, &view, 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  CallXSFixture fx[2] = { { 0 }, { 0 } };
  CTypeID i32_ctype, double_ctype, bad_ctype;
  unsigned i;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[int abs(int); long labs(long); double fabs(double);]]\n"
    "local abs_call = ffi.C.abs\n"
    "local fabs_call = ffi.C.fabs\n"
    "_G.__arm64_callxs_i32 = abs_call\n"
    "_G.__arm64_callxs_double = fabs_call\n"
    "_G.__arm64_callxs_bad = ffi.C.labs\n"
    "function _G.__arm64_callxs_i32_admission(fn, n)\n"
    "  for i = 1, n do\n"
    "    if fn(i) ~= i then return false end\n"
    "  end\n"
    "  return true\n"
    "end\n"
    "function _G.__arm64_callxs_double_admission(fn, n)\n"
    "  for i = 1, n do\n"
    "    if fn(i) ~= i then return false end\n"
    "  end\n"
    "  return true\n"
    "end\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "assert(__arm64_callxs_i32_admission(abs_call, 400))\n"
    "assert(__arm64_callxs_double_admission(fabs_call, 400))\n");

  i32_ctype = global_cdata_ctype(L, "__arm64_callxs_i32");
  double_ctype = global_cdata_ctype(L, "__arm64_callxs_double");
  bad_ctype = global_cdata_ctype(L, "__arm64_callxs_bad");
  assert_scalar_signature(L, i32_ctype, CTID_INT32);
  assert_scalar_signature(L, double_ctype, CTID_DOUBLE);
  assert_unsupported_signature(L, bad_ctype);

  fx[0].profile = CX_PROFILE_I32;
  fx[0].pt = global_lua_proto(L, "__arm64_callxs_i32_admission");
  fx[0].bad_ctype = bad_ctype;
  fx[0].cross_ctype = double_ctype;
  fx[1].profile = CX_PROFILE_DOUBLE;
  fx[1].pt = global_lua_proto(L, "__arm64_callxs_double_admission");
  fx[1].bad_ctype = bad_ctype;
  fx[1].cross_ctype = i32_ctype;

  for (i = 0; i < 2; i++) {
    fx[i].owner = find_exact_callxs_trace(J, &fx[i]);
    if (fx[i].owner == NULL)
      fprintf(stderr, "exact ffi.C.%s CALLXS trace was not published\n",
	  i == 0 ? "abs" : "fabs");
    assert(fx[i].owner != NULL);
    fx[i].ir = (IRIns *)calloc(
	(size_t)trace_nins_acq(fx[i].owner), sizeof(IRIns));
    assert(fx[i].ir != NULL);
  }

  for (i = 0; i < 2; i++) {
    test_semantic(J, &fx[i]);
    test_postra(&fx[i]);
    free(fx[i].ir);
  }

  lua_close(L);
  printf("t-arm64-jit-callxs-admission OK: int/double semantic/post-RA "
	 "mutations rejected\n");
  return 0;
}

#else

int main(void)
{
  printf("t-arm64-jit-callxs-admission SKIP: requires experimental Darwin ARM64\n");
  return 0;
}

#endif
