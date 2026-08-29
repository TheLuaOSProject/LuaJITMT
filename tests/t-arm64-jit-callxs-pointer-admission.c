/*
** Authentic semantic and post-RA admission contract for the first boxed
** pointer CALLXS root on Darwin ARM64. A published getenv trace supplies the
** immutable certificate; private copies are then mutated without execution.
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
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT || !LJ_HASFFI || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-callxs-pointer-admission requires experimental ARM64 CALLXS"
#endif

/* Mirror the deliberately private pointer geometry in
** lj_asm_arm64_admit.h. Keep it local to the mutation fixture. */
enum {
  PX_K_ZERO = REF_TRUE-19u,
  PX_K_TRACE = REF_TRUE-18u,
  PX_K_PAYLOAD_OFS = REF_TRUE-16u,
  PX_K_BOX_CTYPE = REF_TRUE-14u,
  PX_K_STRING_OFS = REF_TRUE-13u,
  PX_K_CTYPE = REF_TRUE-11u,
  PX_K_FTSZ = REF_TRUE-10u,
  PX_K_META = REF_TRUE-8u,
  PX_K_KEY = REF_TRUE-6u,
  PX_K_TABLE = REF_TRUE-4u,
  PX_K_LIMITMAX = REF_TRUE-2u,
  PX_K_ONE = REF_TRUE-1u,

  PX_R_LIMIT = REF_FIRST,
  PX_R_LIMIT_GUARD,
  PX_R_INDEX,
  PX_R_FUNC,
  PX_R_STRING,
  PX_R_MT,
  PX_R_MT_GUARD,
  PX_R_TABLE_ROOT,
  PX_R_KEY_ROOT,
  PX_R_LOOKUP_ARGS,
  PX_R_LOOKUP_OUT,
  PX_R_LOOKUP,
  PX_R_MOBJ,
  PX_R_MOBJ_GUARD,
  PX_R_CTYPE,
  PX_R_CTYPE_GUARD,
  PX_R_FUNCPTR,
  PX_R_STRING_PTR,
  PX_R_BOX_PRE,
  PX_R_PAYLOAD_PRE,
  PX_R_XSAVE_PRE,
  PX_R_ENTER_ARGS,
  PX_R_ENTER_ROOT_PRE,
  PX_R_ENTER_PRE,
  PX_R_ENTER_GUARD_PRE,
  PX_R_CALL_PRE,
  PX_R_STORE_PRE,
  PX_R_LEAVE_PRE,
  PX_R_LEAVE_GUARD_PRE,
  PX_R_RESULT_PRE,
  PX_R_INDEX_PRE,
  PX_R_BOUND_GUARD_PRE,
  PX_R_LOOP,
  PX_R_XPOLL,
  PX_R_MT_BODY,
  PX_R_MT_GUARD_BODY,
  PX_R_TABLE_ROOT_BODY,
  PX_R_KEY_ROOT_BODY,
  PX_R_LOOKUP_ARGS_BODY,
  PX_R_LOOKUP_OUT_BODY,
  PX_R_LOOKUP_BODY,
  PX_R_MOBJ_BODY,
  PX_R_MOBJ_GUARD_BODY,
  PX_R_BOX_BODY,
  PX_R_PAYLOAD_BODY,
  PX_R_XSAVE_BODY,
  PX_R_ENTER_ROOT_BODY,
  PX_R_ENTER_BODY,
  PX_R_ENTER_GUARD_BODY,
  PX_R_CALL_BODY,
  PX_R_STORE_BODY,
  PX_R_LEAVE_BODY,
  PX_R_LEAVE_GUARD_BODY,
  PX_R_INDEX_BODY,
  PX_R_BOUND_GUARD_BODY,
  PX_R_INDEX_PHI,
  PX_R_RESULT_PHI,
  PX_SEMANTIC_NINS,
  PX_PUBLISHED_NINS = PX_SEMANTIC_NINS+2u,

  PX_NSNAP = 18,
  PX_NSNAPMAP = 138,
  PX_ROOTED_PRE_MAP = 41
};

LJ_STATIC_ASSERT(LJ_FR2 == 1);
LJ_STATIC_ASSERT(PX_R_BOX_PRE == REF_BASE+19u);
LJ_STATIC_ASSERT(PX_R_CALL_PRE == REF_BASE+26u);
LJ_STATIC_ASSERT(PX_R_XSAVE_BODY == REF_BASE+46u);
LJ_STATIC_ASSERT(PX_R_CALL_BODY == REF_BASE+50u);
LJ_STATIC_ASSERT(PX_R_RESULT_PHI == REF_BASE+57u);
LJ_STATIC_ASSERT(PX_SEMANTIC_NINS == REF_BASE+58u);

typedef struct PointerFixture {
  GCtrace *owner;
  GCproto *pt;
  IRIns *ir;
  SnapShot snap[PX_NSNAP];
  SnapEntry snapmap[PX_NSNAPMAP];
  GCtrace semantic;
  CTypeID fn_ctype;
  CTypeID bad_ctype;
} PointerFixture;

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

static void assert_pointer_signature(lua_State *L, CTypeID id)
{
  CTState *cts = ctype_ctsG(G(L));
  CType fn, arg, ptr;
  CTypeID fid;
  CTInfo info, ainfo, pinfo;
  assert(cts != NULL && lj_ctype_snapshot(cts, id, &fn) > 0);
  info = ctype_info_acq(&fn);
  assert(ctype_isfunc(info) && ctype_size_acq(&fn) == 1 &&
	 (info & CTF_VARARG) == 0 && ctype_cconv(info) == CTCC_CDECL &&
	 ctype_cid(info) == CTID_P_CCHAR);
  fid = ctype_sib_acq(&fn);
  assert(fid != 0 && lj_ctype_snapshot(cts, fid, &arg) > 0);
  ainfo = ctype_info_acq(&arg);
  assert(ctype_isfield(ainfo) && ctype_cid(ainfo) == CTID_P_CCHAR &&
	 ctype_sib_acq(&arg) == 0);
  assert(lj_ctype_snapshot(cts, CTID_P_CCHAR, &ptr) > 0);
  pinfo = ctype_info_acq(&ptr);
  assert(ctype_isptr(pinfo) && ctype_size_acq(&ptr) == CTSIZE_PTR &&
	 ctype_cid(pinfo) == CTID_CCHAR);
}

static void assert_bad_signature(lua_State *L, CTypeID id)
{
  CTState *cts = ctype_ctsG(G(L));
  CType fn, arg;
  CTypeID fid;
  CTInfo info, ainfo;
  assert(cts != NULL && lj_ctype_snapshot(cts, id, &fn) > 0);
  info = ctype_info_acq(&fn);
  assert(ctype_isfunc(info) && ctype_size_acq(&fn) == 1 &&
	 (info & CTF_VARARG) == 0 && ctype_cconv(info) == CTCC_CDECL &&
	 ctype_cid(info) != CTID_P_CCHAR);
  fid = ctype_sib_acq(&fn);
  assert(fid != 0 && lj_ctype_snapshot(cts, fid, &arg) > 0);
  ainfo = ctype_info_acq(&arg);
  assert(ctype_isfield(ainfo) && ctype_cid(ainfo) == CTID_P_CCHAR &&
	 ctype_sib_acq(&arg) == 0);
}

static GCtrace *find_exact_pointer_trace(jit_State *J, PointerFixture *fx)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    const IRIns *ir;
    IRIns ktrace, rename0, rename1;
    if (!trace_runnable_acq(T, traceno) || trace_root_acq(T) != 0 ||
	trace_startpt_acq(T) != fx->pt ||
	bc_op(trace_startins_acq(T)) != BC_FORL ||
	trace_startpc_acq(T) != proto_bc(fx->pt)+10 ||
	trace_nins_acq(T) != PX_PUBLISHED_NINS ||
	trace_nk_acq(T) != PX_K_ZERO || trace_nsnap_acq(T) != PX_NSNAP ||
	trace_nsnapmap_acq(T) != PX_NSNAPMAP ||
	trace_spadjust_acq(T) != 16 || trace_topslot_acq(T) != 11 ||
	trace_op_count(T, IR_XSAVE) != 2 ||
	trace_op_count(T, IR_CALLXS) != 2 ||
	trace_op_count(T, IR_CALLS) != 6 ||
	trace_op_count(T, IR_TMPREF) != 4 ||
	trace_op_count(T, IR_VLOAD) != 2 ||
	trace_op_count(T, IR_CNEW) != 2 ||
	trace_op_count(T, IR_XSTORE) != 2 ||
	trace_op_count(T, IR_PHI) != 2 ||
	trace_op_count(T, IR_RENAME) != 2)
      continue;
    ir = trace_ir_acq(T);
    ktrace = ir_load_acq(&ir[PX_K_TRACE]);
    rename0 = ir_load_acq(&ir[PX_SEMANTIC_NINS]);
    rename1 = ir_load_acq(&ir[PX_SEMANTIC_NINS+1u]);
    if (ktrace.o != IR_KGC || ktrace.t.irt != IRT_P64 ||
	ir_kgc_load_acq(&ir[PX_K_TRACE]) != obj2gco(T) ||
	ir_load_acq(&ir[PX_K_CTYPE]).i != (int32_t)fx->fn_ctype ||
	ir_load_acq(&ir[PX_K_BOX_CTYPE]).i != CTID_P_CCHAR ||
	rename0.o != IR_RENAME || rename0.t.irt != IRT_NIL ||
	rename0.op1 != PX_R_BOX_PRE || rename0.op2 != 10 ||
	rename0.r != RID_X27 || rename0.s != SPS_NONE ||
	rename1.o != IR_RENAME || rename1.t.irt != IRT_NIL ||
	rename1.op1 != PX_R_BOX_PRE || rename1.op2 != 10 ||
	rename1.r != RID_X0 || rename1.s != SPS_NONE)
      continue;
    return T;
  }
  return NULL;
}

static void reset_published_clone(PointerFixture *fx)
{
  IRRef nk = trace_nk_acq(fx->owner);
  IRRef nins = trace_nins_acq(fx->owner);
  memset(fx->ir, 0, (size_t)nins*sizeof(IRIns));
  memcpy(&fx->ir[nk], &trace_ir_acq(fx->owner)[nk],
	 (size_t)(nins-nk)*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner), sizeof(fx->snap));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner), sizeof(fx->snapmap));
}

static void reset_semantic_clone(PointerFixture *fx)
{
  IRIns *ktrace;
  reset_published_clone(fx);
  fx->semantic = *fx->owner;
  fx->semantic.ir = fx->ir;
  fx->semantic.snap = fx->snap;
  fx->semantic.snapmap = fx->snapmap;
  fx->semantic.nins = PX_SEMANTIC_NINS;

  /* Reconstruct lj_ir_ktrace(). Its payload is deliberately unspecified
  ** until asm_patch_ktrace() publishes the exact body owner. */
  ktrace = &fx->ir[PX_K_TRACE];
  ktrace[1].tv.u64 = UINT64_C(0xa5a55a5a12345678);
  ktrace->op12 = 0;
  ktrace->t.irt = IRT_P64;
  ktrace->o = IR_KNUM;
  ktrace->prev = 0;
}

static SemanticJSave semantic_j_enter(jit_State *J, PointerFixture *fx)
{
  SemanticJSave save = {
    J->curfinal, J->pt, J->startpc, J->loopref, J->parent, J->exitno,
    J->baseslot, J->framedepth, J->retdepth, J->ktrace
  };
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = proto_bc(fx->pt)+10;
  J->loopref = PX_R_LOOP;
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1+LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = PX_K_TRACE;
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

static void expect_semantic_admit(jit_State *J, PointerFixture *fx)
{
  LJArm64IRReject reject;
  int admitted = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (!admitted)
    fprintf(stderr, "pointer CALLXS semantic positive rejected: "
	    "reason=%d ref=%u op=%u detail=%u\n", (int)reject.reason,
	    (unsigned)reject.ref, (unsigned)reject.op,
	    (unsigned)reject.detail);
  assert(admitted && reject.reason == LJ_ARM64_IR_REJECT_NONE);
}

static void expect_semantic_reject(jit_State *J, PointerFixture *fx,
	LJArm64IRRejectReason reason, IRRef ref, IROp op, uint16_t detail)
{
  LJArm64IRReject reject;
  int admitted = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (admitted || reject.reason != reason || reject.ref != ref ||
	reject.op != op || reject.detail != detail)
    fprintf(stderr, "pointer CALLXS semantic reject mismatch: admitted=%d "
	    "wanted=%d/%u/%u/%u got=%d/%u/%u/%u\n", admitted,
	    (int)reason, (unsigned)ref, (unsigned)op, (unsigned)detail,
	    (int)reject.reason, (unsigned)reject.ref, (unsigned)reject.op,
	    (unsigned)reject.detail);
  assert(!admitted && reject.reason == reason && reject.ref == ref &&
	 reject.op == op && reject.detail == detail);
}

static LJArm64PostRAView make_postra_view(PointerFixture *fx)
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

static void expect_postra(LJArm64PostRAView *view, int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_postra_admit(view, &semantic_nins);
  if (result != admitted)
    fprintf(stderr, "pointer CALLXS post-RA mismatch: wanted=%d got=%d\n",
	    admitted, result);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == PX_SEMANTIC_NINS);
}

static void test_semantic(jit_State *J, PointerFixture *fx)
{
  const BCIns *bc = proto_bc(fx->pt);
  BCIns saved_start = loadbc(&bc[10]);
  BCIns saved_call;
  SemanticJSave save;
  IRRef saved_loopref;
  uint64_t pcbase;

  assert(bc_op(fx->owner->startins) == BC_FORL);
  bc_publish((const uint32_t *)&bc[10], fx->owner->startins);
  save = semantic_j_enter(J, fx);

  reset_semantic_clone(fx);
  expect_semantic_admit(J, fx);

  reset_semantic_clone(fx);
  ir_kgc_publish(&fx->ir[PX_K_TRACE], obj2gco(fx->owner), IRT_P64);
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_K_TRACE,
	IR_KGC, 7);

  reset_semantic_clone(fx);
  fx->ir[PX_K_CTYPE].i = (int32_t)fx->bad_ctype;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_K_CTYPE,
	IR_KINT, 8);

  reset_semantic_clone(fx);
  fx->ir[PX_K_BOX_CTYPE].i = CTID_P_UINT8;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_K_TRACE,
	IR_KGC, 7);

  reset_semantic_clone(fx);
  fx->ir[PX_K_PAYLOAD_OFS+1u].tv.u64 ^= UINT64_C(1);
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_K_TRACE,
	IR_KGC, 7);

  reset_semantic_clone(fx);
  fx->semantic.nins--;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_R_CALL_PRE,
	IR_CALLXS, 2);

  reset_semantic_clone(fx);
  saved_loopref = J->loopref;
  J->loopref = PX_R_LOOP-1u;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_R_LOOP,
	IR_LOOP, 5);
  J->loopref = saved_loopref;

  reset_semantic_clone(fx);
  saved_call = loadbc(&bc[8]);
  assert(bc_op(saved_call) == BC_CALL && bc_b(saved_call) == 2);
  bc_publish((const uint32_t *)&bc[8],
	BCINS_ABC(BC_CALL, bc_a(saved_call), 3, bc_c(saved_call)));
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_R_LOOP,
	IR_LOOP, 9);
  bc_publish((const uint32_t *)&bc[8], saved_call);

  reset_semantic_clone(fx);
  fx->ir[PX_R_STRING_PTR].op2 = PX_K_PAYLOAD_OFS;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_R_CALL_PRE,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[PX_R_STORE_PRE].op1 = PX_R_STRING_PTR;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_R_CALL_PRE,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[PX_R_BOX_BODY].t.irt &= (uint8_t)~IRT_ISPHI;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_R_CALL_PRE,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  fx->ir[PX_R_RESULT_PHI].op1 = PX_R_RESULT_PRE;
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_CALL, PX_R_CALL_PRE,
	IR_CALLXS, 10);

  reset_semantic_clone(fx);
  assert(fx->snapmap[PX_ROOTED_PRE_MAP] ==
	 SNAP(14, 0, PX_R_BOX_PRE));
  fx->snapmap[PX_ROOTED_PRE_MAP] = SNAP(14, 0, PX_R_STRING);
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_SNAPSHOT,
	PX_R_XSAVE_PRE, IR_XSAVE, 11);

  reset_semantic_clone(fx);
  LJ_STATIC_ASSERT(sizeof(pcbase) == sizeof(SnapEntry)*(1+LJ_FR2));
  memcpy(&pcbase, &fx->snapmap[42], sizeof(pcbase));
  pcbase ^= UINT64_C(1);
  memcpy(&fx->snapmap[42], &pcbase, sizeof(pcbase));
  expect_semantic_reject(J, fx, LJ_ARM64_IR_REJECT_SNAPSHOT,
	PX_R_XSAVE_PRE, IR_XSAVE, 11);

  semantic_j_leave(J, &save);
  bc_publish((const uint32_t *)&bc[10], saved_start);
}

static void test_postra(PointerFixture *fx)
{
  const BCIns *bc = proto_bc(fx->pt);
  LJArm64PostRAView view;
  GCtrace decoy = *fx->owner;
  BCIns saved_start;
  TraceNo owner_no = trace_traceno_acq(fx->owner);
  TraceNo wrong_no = owner_no == 1 ? 2 : 1;

  view = make_postra_view(fx);
  assert(view.nins == PX_PUBLISHED_NINS && view.nk == PX_K_ZERO &&
	 view.nsnap == PX_NSNAP && view.nsnapmap == PX_NSNAPMAP &&
	 view.spadjust == 16 && view.root_topslot == 11 &&
	 view.proto_sizebc == 13 && view.proto_numparams == 3);
  expect_postra(&view, 1);

  view = make_postra_view(fx);
  view.owner = &decoy;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  saved_start = loadbc(&bc[10]);
  assert(bc_op(saved_start) == BC_JFORL && wrong_no != owner_no);
  bc_publish((const uint32_t *)&bc[10],
	BCINS_AD(BC_JFORL, bc_a(view.startins), wrong_no));
  expect_postra(&view, 0);
  bc_publish((const uint32_t *)&bc[10], saved_start);

  view = make_postra_view(fx);
  view.nins--;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  view.spadjust = 0;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  assert(fx->ir[PX_SEMANTIC_NINS].op1 == PX_R_BOX_PRE &&
	 fx->ir[PX_SEMANTIC_NINS].op2 == 10 &&
	 fx->ir[PX_SEMANTIC_NINS].r == RID_X27);
  fx->ir[PX_SEMANTIC_NINS].op2 = 9;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  assert(fx->ir[PX_SEMANTIC_NINS+1u].r == RID_X0);
  fx->ir[PX_SEMANTIC_NINS+1u].r = RID_X1;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  assert(fx->ir[PX_R_FUNCPTR].r == RID_X20 &&
	 fx->ir[PX_R_FUNCPTR].s == SPS_NONE);
  fx->ir[PX_R_FUNCPTR].r = RID_X0;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  assert(fx->ir[PX_R_BOX_PRE].r == RID_X27 &&
	 fx->ir[PX_R_BOX_PRE].s == 6);
  fx->ir[PX_R_BOX_PRE].s = 4;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  assert(fx->ir[PX_K_BOX_CTYPE].r == RID_INIT &&
	 fx->ir[PX_K_BOX_CTYPE].s == SPS_NONE);
  fx->ir[PX_K_BOX_CTYPE].r = RID_X0;
  expect_postra(&view, 0);

  view = make_postra_view(fx);
  assert(fx->snapmap[PX_ROOTED_PRE_MAP] ==
	 SNAP(14, 0, PX_R_BOX_PRE));
  fx->snapmap[PX_ROOTED_PRE_MAP] = SNAP(14, 0, PX_R_BOX_BODY);
  expect_postra(&view, 0);
}

int main(void)
{
  lua_State *L;
  jit_State *J;
  PointerFixture fx = { 0 };

  assert(setenv("LJ_M7_CALLXS_POINTER_ADMISSION", "pointer-admission", 1) == 0);
  L = ljt_lua_newstate_openlibs();
  J = G2J(G(L));
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "const char *getenv(const char *);\n"
    "int atoi(const char *);\n"
    "]]\n"
    "local getenv_call = ffi.C.getenv\n"
    "_G.__arm64_callxs_pointer = getenv_call\n"
    "_G.__arm64_callxs_pointer_bad = ffi.C.atoi\n"
    "function _G.__arm64_callxs_pointer_admission(fn, n, p)\n"
    "  local value\n"
    "  for _ = 1, n do value = fn(p) end\n"
    "  return value\n"
    "end\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local result = __arm64_callxs_pointer_admission(\n"
    "  getenv_call, 400, 'LJ_M7_CALLXS_POINTER_ADMISSION')\n"
    "assert(ffi.string(result) == 'pointer-admission')\n");

  fx.pt = global_lua_proto(L, "__arm64_callxs_pointer_admission");
  fx.fn_ctype = global_cdata_ctype(L, "__arm64_callxs_pointer");
  fx.bad_ctype = global_cdata_ctype(L, "__arm64_callxs_pointer_bad");
  assert_pointer_signature(L, fx.fn_ctype);
  assert_bad_signature(L, fx.bad_ctype);
  fx.owner = find_exact_pointer_trace(J, &fx);
  if (fx.owner == NULL)
    fprintf(stderr, "exact ffi.C.getenv pointer CALLXS trace was not published\n");
  assert(fx.owner != NULL);
  fx.ir = (IRIns *)calloc((size_t)trace_nins_acq(fx.owner), sizeof(IRIns));
  assert(fx.ir != NULL);

  test_semantic(J, &fx);
  test_postra(&fx);

  free(fx.ir);
  lua_close(L);
  printf("t-arm64-jit-callxs-pointer-admission OK: authentic pointer "
	 "semantic/post-RA mutations rejected\n");
  return 0;
}

#else

int main(void)
{
  printf("t-arm64-jit-callxs-pointer-admission SKIP: requires experimental "
	 "Darwin ARM64\n");
  return 0;
}

#endif
