/* Native Darwin ARM64 CALLXS ABI-class matrix and classifier negatives. */
#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
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
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ccall_arm64.h"
#include "lj_ctype.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_tg.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT || !LJ_HASFFI || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED
#error "FFI ABI matrix requires experimental ARM64 CALLXS admission"
#endif

typedef void (*InstallObserver)(void (*fn)(unsigned));
typedef unsigned (*ReadCounter)(unsigned id);

static TGState *test_tg;
enum { ABI_TEST_MAX_ID = 15 };
static unsigned native_seen[ABI_TEST_MAX_ID];
static void *expected_func[ABI_TEST_MAX_ID];
static unsigned finish_target, forced_finishes;
static uint64_t finish_old_epoch;
static CTypeID global_ctype(lua_State *L, const char *name);

typedef struct SemanticClone {
  GCtrace trace;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef ktrace;
} SemanticClone;

typedef struct SemanticJSave {
  GCtrace *curfinal;
  GCproto *pt;
  const BCIns *startpc;
  IRRef loopref;
  TraceNo parent;
  ExitNo exitno;
  BCReg baseslot;
  int32_t framedepth, retdepth;
  IRRef ktrace;
} SemanticJSave;

static void semantic_clone_init(SemanticClone *clone, GCtrace *owner)
{
  IRRef nins = trace_nins_acq(owner), nk = trace_nk_acq(owner);
  IRRef ref;
  MSize nsnap = trace_nsnap_acq(owner);
  MSize nsnapmap = trace_nsnapmap_acq(owner);
  memset(clone, 0, sizeof(*clone));
  clone->ir = (IRIns *)calloc(nins, sizeof(IRIns));
  clone->snap = (SnapShot *)malloc(nsnap*sizeof(SnapShot));
  clone->snapmap = (SnapEntry *)malloc(nsnapmap*sizeof(SnapEntry));
  assert(clone->ir != NULL && clone->snap != NULL &&
         clone->snapmap != NULL);
  assert(nk < REF_FIRST && nins >= REF_FIRST && nsnap != 0 &&
         nsnapmap >= 1u+LJ_FR2);
  memcpy(&clone->ir[nk], &trace_ir_acq(owner)[nk],
         (nins-nk)*sizeof(IRIns));
  memcpy(clone->snap, trace_snap_acq(owner), nsnap*sizeof(SnapShot));
  memcpy(clone->snapmap, trace_snapmap_acq(owner),
         nsnapmap*sizeof(SnapEntry));
  clone->trace = *owner;
  clone->trace.ir = clone->ir;
  clone->trace.snap = clone->snap;
  clone->trace.snapmap = clone->snapmap;
  clone->ktrace = 0;
  for (ref = nk; ref < REF_TRUE; ) {
    IRIns k = ir_load_acq(&clone->ir[ref]);
    IRRef next = ref+1u;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
                     k.o == IR_KKPTR)))
      next = ref+2u <= REF_TRUE ? ref+2u : REF_DROP;
    if (k.o == IR_KGC && ir_kgc_load_acq(&clone->ir[ref]) == obj2gco(owner)) {
      assert(clone->ktrace == 0 && k.t.irt == IRT_P64 &&
             ref+1u < REF_TRUE);
      clone->ktrace = ref;
      clone->ir[ref+1u].tv.u64 = 0;
      clone->ir[ref].op12 = 0;
      clone->ir[ref].t.irt = IRT_P64;
      clone->ir[ref].o = IR_KNUM;
      clone->ir[ref].prev = 0;
    }
    assert(next > ref && next <= REF_TRUE);
    ref = next;
  }
  assert(clone->ktrace >= nk && clone->ktrace < REF_TRUE);
  /* Drop the post-RA-only RENAME/NOP suffix. */
  while (clone->trace.nins > REF_FIRST) {
    IRRef ref = clone->trace.nins-1u;
    IRIns tail = ir_load_acq(&clone->ir[ref]);
    if (tail.o == IR_RENAME ||
        (tail.o == IR_NOP && tail.t.irt == IRT_NIL &&
         tail.op1 == 0 && tail.op2 == 0 && tail.prev == 0))
      clone->trace.nins = ref;
    else
      break;
  }
}

static void semantic_clone_free(SemanticClone *clone)
{
  free(clone->snapmap);
  free(clone->snap);
  free(clone->ir);
  memset(clone, 0, sizeof(*clone));
}

static SemanticJSave semantic_j_enter(jit_State *J, GCproto *pt,
                                      GCtrace *T, IRRef ktrace)
{
  SemanticJSave save = { J->curfinal, J->pt, J->startpc, J->loopref,
    J->parent, J->exitno, J->baseslot, J->framedepth, J->retdepth,
    J->ktrace };
  IRRef ref;
  J->curfinal = NULL;
  J->pt = pt;
  J->startpc = trace_startpc_acq(T);
  J->loopref = 0;
  for (ref = REF_BASE; ref < T->nins; ref++)
    if (T->ir[ref].o == IR_LOOP) {
      J->loopref = ref;
      break;
    }
  assert(J->loopref != 0);
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1+LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = ktrace;
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

static void expect_semantic_admit(jit_State *J, GCtrace *T)
{
  LJArm64IRReject reject;
  uint32_t *startpc = (uint32_t *)trace_startpc_acq(T);
  uint32_t liveins = la_load32_acq(startpc);
  int accepted;
  /* The semantic checker runs at the pre-publication boundary, where the
  ** source FORL is still present. Restore that one word only for the check. */
  la_store32_rel(startpc, T->startins);
  accepted = lj_asm_arm64_ir_admit(J, T, &reject);
  la_store32_rel(startpc, liveins);
  if (!accepted)
    fprintf(stderr, "ABI footer semantic baseline rejected: reason=%d "
            "ref=%u op=%u detail=%u\n", (int)reject.reason,
            (unsigned)reject.ref, (unsigned)reject.op,
            (unsigned)reject.detail);
  assert(accepted && reject.reason == LJ_ARM64_IR_REJECT_NONE);
}

static void expect_semantic_reject(jit_State *J, GCtrace *T,
                                   const char *mutation)
{
  LJArm64IRReject reject;
  uint32_t *startpc = (uint32_t *)trace_startpc_acq(T);
  uint32_t liveins = la_load32_acq(startpc);
  int accepted;
  la_store32_rel(startpc, T->startins);
  accepted = lj_asm_arm64_ir_admit(J, T, &reject);
  la_store32_rel(startpc, liveins);
  if (accepted)
    fprintf(stderr, "ABI footer semantic mutation accepted: %s\n", mutation);
  assert(!accepted && reject.reason == LJ_ARM64_IR_REJECT_SNAPSHOT &&
         reject.op == IR_XSAVE);
}

static void expect_semantic_reject_any(jit_State *J, GCtrace *T,
                                       const char *mutation)
{
  LJArm64IRReject reject;
  uint32_t *startpc = (uint32_t *)trace_startpc_acq(T);
  uint32_t liveins = la_load32_acq(startpc);
  int accepted;
  la_store32_rel(startpc, T->startins);
  accepted = lj_asm_arm64_ir_admit(J, T, &reject);
  la_store32_rel(startpc, liveins);
  if (accepted)
    fprintf(stderr, "ABI stack semantic mutation accepted: %s\n", mutation);
  assert(!accepted && reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

static int fixture_callxs_carg_values(const IRIns *ir, IRRef nk,
                                      IRRef nins, IRRef call,
                                      unsigned nargs, IRRef *values)
{
  IRIns ins;
  IRRef ref;
  unsigned i;
  if (call < REF_FIRST || call >= nins || values == NULL ||
      nargs > LJ_ARM64_ABI_MAX_ARGS)
    return 0;
  ins = ir_load_acq(&ir[call]);
  if (ins.o != IR_CALLXS) return 0;
  ref = ins.op1;
  for (i = nargs; i > 1u; i--) {
    IRIns carg;
    if (ref < REF_FIRST || ref >= call) return 0;
    carg = ir_load_acq(&ir[ref]);
    if (carg.o != IR_CARG || carg.t.irt != IRT_NIL ||
        carg.op1 < nk || carg.op1 >= ref ||
        carg.op2 < nk || carg.op2 >= ref)
      return 0;
    values[i-1u] = carg.op2;
    ref = carg.op1;
  }
  if (nargs == 0) return ref == 0;
  if (ref < nk || ref >= call) return 0;
  values[0] = ref;
  return 1;
}

static IRRef fixture_callxs_carg_for_argument(const IRIns *ir, IRRef call,
                                      unsigned nargs, unsigned argindex)
{
  IRRef ref = ir_load_acq(&ir[call]).op1;
  unsigned i;
  assert(argindex > 0 && argindex < nargs);
  for (i = nargs-1u; i > argindex; i--) {
    IRIns carg = ir_load_acq(&ir[ref]);
    assert(carg.o == IR_CARG && carg.op1 < ref);
    ref = carg.op1;
  }
  assert(ir_load_acq(&ir[ref]).o == IR_CARG);
  return ref;
}

static int fixture_callxs_signature(CTState *cts, const IRIns *ir,
                                    IRRef nk, IRRef nins, IRRef call,
                                    CTypeID expected, LJArm64ABISignature *abi,
                                    IRRef *values)
{
  IRIns c, fp;
  IRRef cdref, fpref, ref;
  int found = 0;
  if (call < REF_FIRST || call >= nins ||
      (c = ir_load_acq(&ir[call])).o != IR_CALLXS ||
      (fpref = c.op2) < REF_FIRST || fpref >= call)
    return 0;
  fp = ir_load_acq(&ir[fpref]);
  if (fp.o != IR_FLOAD || fp.op2 != IRFL_CDATA_PTR ||
      (cdref = fp.op1) < REF_FIRST || cdref >= fpref ||
      ir_load_acq(&ir[cdref]).o != IR_SLOAD)
    return 0;
  for (ref = REF_FIRST; ref < call; ref++) {
    IRIns id = ir_load_acq(&ir[ref]);
    IRRef guard;
    if (id.o != IR_FLOAD || id.op1 != cdref ||
        id.op2 != IRFL_CDATA_CTYPEID)
      continue;
    for (guard = ref+1u; guard < call; guard++) {
      IRIns eq = ir_load_acq(&ir[guard]);
      IRRef kref;
      IRIns k;
      if (eq.o != IR_EQ || !irt_isguard(eq.t) ||
          (eq.op1 != ref && eq.op2 != ref))
        continue;
      kref = eq.op1 == ref ? eq.op2 : eq.op1;
      if (kref < nk || kref >= REF_TRUE) return 0;
      k = ir_load_acq(&ir[kref]);
      if (k.o != IR_KINT || (CTypeID)k.i != expected) continue;
      if (found || !lj_ccall_arm64_signature(cts, expected, abi) ||
          !fixture_callxs_carg_values(ir, nk, nins, call, abi->nargs,
                                      values))
        return 0;
      found = 1;
      break;
    }
  }
  return found;
}

static int trace_has_stack_store(const GCtrace *T, uint32_t opcode,
                                 unsigned scale, unsigned offset)
{
  MCode *code = trace_mcode_acq(T);
  MSize bytes = trace_szmcode_acq(T);
  MSize i;
  for (i = 0; i+sizeof(MCode) <= bytes; i += sizeof(MCode)) {
    uint32_t ins = (uint32_t)code[i/sizeof(MCode)];
    unsigned rn = (ins >> 5) & 31u;
    unsigned imm12 = (ins >> 10) & 0xfffu;
    if ((ins & 0xffc00000u) == (opcode & 0xffc00000u) && rn == 31u &&
        (imm12 << scale) == offset)
      return 1;
  }
  return 0;
}

/* Authenticate the stack-argument trace independently at both boundaries.
** The two byte stores and three naturally aligned word stores have distinct
** widths/offsets, so unrelated STP spill pairs cannot satisfy these checks. */
static void test_stack_call_admission(lua_State *L)
{
  jit_State *J = G2J(G(L));
  CTState *cts = ctype_ctsG(G(L));
  GCfunc *fn;
  GCproto *pt;
  const BCIns *bc;
  uintptr_t lo, hi;
  TraceVec *tv;
  GCtrace *T = NULL;
  IRRef compact_call = 0, narrow8 = 0, narrow_carg = 0;
  IRRef narrow16_carg = 0, narrow16 = 0;
  IRRef refs[LJ_ARM64_ABI_MAX_ARGS];
  MSize trace_no;
  IRRef ref;
  LJArm64PostRAView view = { 0 };
  IRIns *copy;
  SemanticClone semantic;
  SemanticJSave jsave;
  CTypeID compact_id;
  LJArm64ABISignature compact_abi;
  IRRef spillref = 0;

  lua_getglobal(L, "__abi_compact");
  assert(tviscdata(L->top-1));
  compact_id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  assert(lj_ccall_arm64_signature(cts, compact_id, &compact_abi));
  lua_getglobal(L, "__abi_run_stack_matrix");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  lua_pop(L, 1);
  bc = proto_bc(pt);
  lo = (uintptr_t)(const void *)bc;
  hi = lo+(uintptr_t)pt->sizebc*sizeof(BCIns);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (trace_no = 1; trace_no < tv->sizetrace && T == NULL; trace_no++) {
    GCtrace *candidate = traceref_safe(J, trace_no);
    if (candidate == NULL || trace_startpc_acq(candidate) == NULL ||
        (uintptr_t)(const void *)trace_startpc_acq(candidate) < lo ||
        (uintptr_t)(const void *)trace_startpc_acq(candidate) >= hi)
      continue;
    for (ref = trace_nk_acq(candidate); ref < trace_nins_acq(candidate); ref++) {
      LJArm64ABISignature abi;
      if (ir_load_acq(&trace_ir_acq(candidate)[ref]).o != IR_CALLXS ||
          !fixture_callxs_signature(cts, trace_ir_acq(candidate),
              trace_nk_acq(candidate), trace_nins_acq(candidate), ref,
              compact_id, &abi, refs) || abi.nargs != 21 ||
          abi.result_id != compact_abi.result_id ||
          abi.arg[16].id != compact_abi.arg[16].id ||
          abi.arg[17].id != compact_abi.arg[17].id ||
          abi.arg[19].id != compact_abi.arg[19].id)
        continue;
      if (abi.arg[16].size != 1 || abi.arg[17].size != 2 ||
          abi.arg[18].size != 8 || abi.arg[19].kind != LJ_ARM64_ABI_KIND_POINTER)
        continue;
      T = candidate;
      compact_call = ref;
      narrow8 = refs[16];
      narrow16 = refs[17];
      narrow_carg = fixture_callxs_carg_for_argument(trace_ir_acq(T), ref,
                                                     abi.nargs, 16);
      narrow16_carg = fixture_callxs_carg_for_argument(trace_ir_acq(T), ref,
                                                       abi.nargs, 17);
      break;
    }
  }
  assert(T != NULL && compact_call != 0 && narrow8 != 0 && narrow16 != 0);
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = bc;
  view.proto = pt;
  view.startpc = trace_startpc_acq(T);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.proto_numparams = pt->numparams;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  assert(view.spadjust >= 32u);
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  assert(trace_has_stack_store(T, A64I_STRB, 0, 0));
  assert(trace_has_stack_store(T, A64I_STRH, 1, 2));
  assert(trace_has_stack_store(T, A64I_STRd, 3, 8));
  assert(trace_has_stack_store(T, A64I_STRx, 3, 16));
  assert(trace_has_stack_store(T, A64I_STRw, 2, 24));

  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic.ir[narrow8].t.irt = IRT_INT;
  expect_semantic_reject_any(J, &semantic.trace,
                             "byte stack carrier widened to INT");
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic.ir[narrow_carg].op1 = REF_NIL;
  expect_semantic_reject_any(J, &semantic.trace,
                             "stack CARG chain shortened");
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic.ir[narrow_carg].op2 = narrow16;
  semantic.ir[narrow16_carg].op2 = narrow8;
  expect_semantic_reject_any(J, &semantic.trace,
                             "byte and halfword stack CARG classes swapped");
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  copy = (IRIns *)calloc(view.nins, sizeof(*copy));
  assert(copy != NULL);
  memcpy(&copy[view.nk], &view.ir[view.nk],
         (view.nins-view.nk)*sizeof(IRIns));
  view.ir = copy;
  copy[narrow8].t.irt = IRT_INT;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  free(copy);
  view.ir = trace_ir_acq(T);

  copy = (IRIns *)calloc(view.nins, sizeof(*copy));
  assert(copy != NULL);
  memcpy(&copy[view.nk], &view.ir[view.nk],
         (view.nins-view.nk)*sizeof(IRIns));
  view.ir = copy;
  copy[narrow_carg].op1 = REF_NIL;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  free(copy);
  view.ir = trace_ir_acq(T);

  copy = (IRIns *)calloc(view.nins, sizeof(*copy));
  assert(copy != NULL);
  memcpy(&copy[view.nk], &view.ir[view.nk],
         (view.nins-view.nk)*sizeof(IRIns));
  view.ir = copy;
  copy[narrow_carg].op2 = narrow16;
  copy[narrow16_carg].op2 = narrow8;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  free(copy);
  view.ir = trace_ir_acq(T);

  {
    LJArm64PostRAView badview = view;
    badview.spadjust = 0;
    assert(!lj_asm_arm64_postra_admit(&badview, NULL));
  }

  for (ref = REF_FIRST; ref < view.nins && !spillref; ref++) {
    IRIns ins = ir_load_acq(&view.ir[ref]);
    IRRef use;
    if (!ra_hasspill(ins.s) || ins.s < SPS_FIRST || ref >= compact_call ||
        ins.o == IR_CARG || ins.o == IR_XSAVE || ins.o == IR_RENAME)
      continue;
    for (use = compact_call+1u; use < view.nins; use++) {
      IRIns later = ir_load_acq(&view.ir[use]);
      if (later.op1 == ref || later.op2 == ref) {
        spillref = ref;
        break;
      }
    }
  }
  assert(spillref != 0);
  copy = (IRIns *)calloc(view.nins, sizeof(*copy));
  assert(copy != NULL);
  memcpy(&copy[view.nk], &view.ir[view.nk],
         (view.nins-view.nk)*sizeof(IRIns));
  view.ir = copy;
  copy[spillref].s = SPS_FIRST;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  free(copy);
}

/* Mutate each exact F32 conversion independently at both CALLXS admission
** boundaries. The ordinary rooted/scalar path is intentionally untouched. */
static void test_float_call_admission(lua_State *L)
{
  jit_State *J = G2J(G(L));
  CTState *cts = ctype_ctsG(G(L));
  GCfunc *fn;
  GCproto *pt;
  CTypeID ctype;
  const BCIns *bc;
  uintptr_t lo, hi;
  TraceVec *tv;
  GCtrace *T = NULL;
  IRRef args[LJ_ARM64_ABI_MAX_ARGS], call = 0;
  IRRef argconv = 0, resconv = 0, ref;
  LJArm64ABISignature abi;
  LJArm64PostRAView view = { 0 };
  SemanticClone semantic;
  SemanticJSave jsave;
  IRIns *copy;
  MSize no;
  unsigned i;

  lua_getglobal(L, "__abi_run_float_result");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  lua_pop(L, 1);
  ctype = global_ctype(L, "__abi_f32");
  bc = proto_bc(pt);
  lo = (uintptr_t)(const void *)bc;
  hi = lo+(uintptr_t)pt->sizebc*sizeof(BCIns);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (no = 1; no < tv->sizetrace && T == NULL; no++) {
    GCtrace *candidate = traceref_safe(J, no);
    if (candidate == NULL || trace_startpc_acq(candidate) == NULL ||
	(uintptr_t)(const void *)trace_startpc_acq(candidate) < lo ||
	(uintptr_t)(const void *)trace_startpc_acq(candidate) >= hi)
      continue;
    for (ref = trace_nk_acq(candidate); ref < trace_nins_acq(candidate); ref++) {
      if (ir_load_acq(&trace_ir_acq(candidate)[ref]).o == IR_CALLXS &&
	  fixture_callxs_signature(cts, trace_ir_acq(candidate),
	    trace_nk_acq(candidate), trace_nins_acq(candidate), ref, ctype,
	    &abi, args)) {
	T = candidate;
	call = ref;
	break;
      }
    }
  }
  assert(T != NULL && call != 0 && abi.nargs == 2);
  for (i = 0; i < abi.nargs; i++) {
    IRIns arg = ir_load_acq(&trace_ir_acq(T)[args[i]]);
    if (arg.o == IR_CONV && arg.t.irt == IRT_FLOAT &&
	arg.op2 == ((IRT_FLOAT << IRCONV_DSH)|IRT_NUM)) {
      argconv = args[i];
      break;
    }
  }
  for (ref = call+1u; ref < trace_nins_acq(T); ref++) {
    IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
    if (ins.o == IR_CONV && ins.op1 == call && ins.t.irt == IRT_NUM &&
	ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT)) {
      resconv = ref;
      break;
    }
  }
  assert(argconv != 0 && resconv != 0);

  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = bc;
  view.proto = pt;
  view.startpc = trace_startpc_acq(T);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.proto_numparams = pt->numparams;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  view.owner_private = 0;
  assert(lj_asm_arm64_postra_admit(&view, NULL));

  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic.ir[argconv].op2 = IRCONV_NUM_INT;
  expect_semantic_reject_any(J, &semantic.trace,
			     "F32 argument conversion source/mode mismatch");
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic.ir[resconv].op2 = IRCONV_NUM_INT;
  expect_semantic_reject_any(J, &semantic.trace,
			     "F32 result conversion source/mode mismatch");
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic.ir[argconv].op2 |= IRCONV_SEXT;
  expect_semantic_reject_any(J, &semantic.trace,
			     "F32 conversion with unsupported sign-extension flag");
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  copy = (IRIns *)calloc(view.nins, sizeof(*copy));
  assert(copy != NULL);
  memcpy(&copy[view.nk], &view.ir[view.nk],
	 (view.nins-view.nk)*sizeof(IRIns));
  view.ir = copy;
  copy[argconv].op2 = IRCONV_NUM_INT;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  memcpy(&copy[view.nk], &trace_ir_acq(T)[view.nk],
	 (view.nins-view.nk)*sizeof(IRIns));
  copy[resconv].op2 = IRCONV_NUM_INT;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  memcpy(&copy[view.nk], &trace_ir_acq(T)[view.nk],
	 (view.nins-view.nk)*sizeof(IRIns));
  copy[argconv].op2 |= IRCONV_SEXT;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  {
    IRRef targets[] = { argconv, argconv, argconv, resconv,
                        resconv, resconv, resconv };
    IRIns bad[7];
    unsigned m;
    for (m = 0; m < 7; m++)
      bad[m] = ir_load_acq(&trace_ir_acq(T)[targets[m]]);
    bad[0].op1 = REF_BASE;  /* Raw pointer source is not a number. */
    bad[1].t.irt = IRT_NUM;
    bad[2].t.irt |= IRT_ISPHI;
    bad[3].op1 = ir_load_acq(&trace_ir_acq(T)[argconv]).op1;
    bad[4].t.irt = IRT_FLOAT;
    bad[5].t.irt |= IRT_ISPHI;
    bad[6].op2 |= IRCONV_SEXT;
    for (m = 0; m < 7; m++) {
      semantic_clone_init(&semantic, T);
      jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
      expect_semantic_admit(J, &semantic.trace);
      semantic.ir[targets[m]] = bad[m];
      expect_semantic_reject_any(J, &semantic.trace,
                                 "F32 conversion type/source/flag mismatch");
      semantic_j_leave(J, &jsave);
      semantic_clone_free(&semantic);
      memcpy(&copy[view.nk], &trace_ir_acq(T)[view.nk],
             (view.nins-view.nk)*sizeof(IRIns));
      copy[targets[m]] = bad[m];
      assert(!lj_asm_arm64_postra_admit(&view, NULL));
    }
    for (m = 0; m < 2; m++) {
      memcpy(&copy[view.nk], &trace_ir_acq(T)[view.nk],
             (view.nins-view.nk)*sizeof(IRIns));
      copy[m ? resconv : argconv].r = RID_X0;
      assert(!lj_asm_arm64_postra_admit(&view, NULL));
    }
  }
  {
    unsigned found = 0;
    /* Certify the result's range checks separately from its conversions. */
    for (ref = call+1u; ref < trace_nins_acq(T); ref++) {
      IRIns original = ir_load_acq(&trace_ir_acq(T)[ref]);
      unsigned m;
      if (original.o != IR_SUB && original.o != IR_ULE &&
          original.o != IR_UGE) continue;
      found |= original.o == IR_SUB ? 1u : original.o == IR_ULE ? 2u : 4u;
      for (m = 0; m < 5; m++) {
        IRIns bad = original;
        if (m == 0) bad.t.irt = IRT_INT | (original.t.irt & IRT_GUARD);
        if (m == 1) bad.t.irt ^= IRT_GUARD;
        if (m == 2) bad.op1 = REF_BASE;
        if (m == 3) bad.op2 = ref;
        if (m == 4) bad.t.irt |= IRT_ISPHI;
        semantic_clone_init(&semantic, T);
        jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
        expect_semantic_admit(J, &semantic.trace);
        semantic.ir[ref] = bad;
        expect_semantic_reject_any(J, &semantic.trace,
                                  "F32 range check type/guard/source mismatch");
        semantic_j_leave(J, &jsave);
        semantic_clone_free(&semantic);
        memcpy(&copy[view.nk], &trace_ir_acq(T)[view.nk],
               (view.nins-view.nk)*sizeof(IRIns));
        copy[ref] = bad;
        assert(!lj_asm_arm64_postra_admit(&view, NULL));
      }
    }
    assert(found == 7u);
  }
  free(copy);
}

static void test_float_stack_emission(lua_State *L)
{
  jit_State *J = G2J(G(L));
  CTState *cts = ctype_ctsG(G(L));
  GCfunc *fn;
  GCproto *pt;
  CTypeID ctype = global_ctype(L, "__abi_fpr9_float");
  const BCIns *bc;
  uintptr_t lo, hi;
  TraceVec *tv;
  GCtrace *T = NULL;
  LJArm64ABISignature abi;
  IRRef args[LJ_ARM64_ABI_MAX_ARGS], ref;
  LJArm64PostRAView view = { 0 };
  SemanticClone semantic;
  SemanticJSave jsave;
  MSize no;

  lua_getglobal(L, "__abi_run_float_stack");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  lua_pop(L, 1);
  bc = proto_bc(pt);
  lo = (uintptr_t)(const void *)bc;
  hi = lo+(uintptr_t)pt->sizebc*sizeof(BCIns);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (no = 1; no < tv->sizetrace && T == NULL; no++) {
    GCtrace *candidate = traceref_safe(J, no);
    if (candidate == NULL || trace_startpc_acq(candidate) == NULL ||
	(uintptr_t)(const void *)trace_startpc_acq(candidate) < lo ||
	(uintptr_t)(const void *)trace_startpc_acq(candidate) >= hi)
      continue;
    for (ref = trace_nk_acq(candidate); ref < trace_nins_acq(candidate); ref++) {
      if (ir_load_acq(&trace_ir_acq(candidate)[ref]).o == IR_CALLXS &&
	  fixture_callxs_signature(cts, trace_ir_acq(candidate),
	    trace_nk_acq(candidate), trace_nins_acq(candidate), ref, ctype,
	    &abi, args)) {
	T = candidate;
	break;
      }
    }
  }
  assert(T != NULL && abi.nargs == 9 &&
	 abi.arg[8].reg == LJ_ARM64_ABI_STACK &&
	 abi.arg[8].stack_offset == 0 && abi.arg[8].size == 4);
  assert(trace_has_stack_store(T, A64I_STRs, 2, 0));
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = bc;
  view.proto = pt;
  view.startpc = trace_startpc_acq(T);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.proto_numparams = pt->numparams;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  view.owner_private = 0;
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);
}

static void test_mixed_float_stack_emission(lua_State *L)
{
  jit_State *J = G2J(G(L));
  CTState *cts = ctype_ctsG(G(L));
  GCfunc *fn;
  GCproto *pt;
  CTypeID ctype = global_ctype(L, "__abi_mixed_f32_stack");
  const BCIns *bc;
  uintptr_t lo, hi;
  TraceVec *tv;
  GCtrace *T = NULL;
  LJArm64ABISignature abi;
  IRRef args[LJ_ARM64_ABI_MAX_ARGS], ref;
  IRRef resultconv = 0;
  IRRef leave = 0;
  unsigned i;
  MSize no;
  LJArm64PostRAView view = { 0 };
  SemanticClone semantic;
  SemanticJSave jsave;

  lua_getglobal(L, "__abi_run_mixed_float_stack");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  lua_pop(L, 1);
  bc = proto_bc(pt);
  lo = (uintptr_t)(const void *)bc;
  hi = lo+(uintptr_t)pt->sizebc*sizeof(BCIns);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (no = 1; no < tv->sizetrace && T == NULL; no++) {
    GCtrace *candidate = traceref_safe(J, no);
    if (candidate == NULL || trace_startpc_acq(candidate) == NULL ||
	(uintptr_t)(const void *)trace_startpc_acq(candidate) < lo ||
	(uintptr_t)(const void *)trace_startpc_acq(candidate) >= hi)
      continue;
    for (ref = trace_nk_acq(candidate); ref < trace_nins_acq(candidate); ref++) {
      if (ir_load_acq(&trace_ir_acq(candidate)[ref]).o == IR_CALLXS &&
	  fixture_callxs_signature(cts, trace_ir_acq(candidate),
	    trace_nk_acq(candidate), trace_nins_acq(candidate), ref, ctype,
	    &abi, args)) {
	T = candidate;
	break;
      }
    }
  }
  assert(T != NULL && abi.nargs == 18 && abi.stack_size == 8);
  assert(abi.arg[16].cls == LJ_ARM64_ABI_GPR &&
	 abi.arg[16].reg == LJ_ARM64_ABI_STACK &&
	 abi.arg[16].stack_offset == 0 && abi.arg[16].size == 4);
  assert(abi.arg[17].cls == LJ_ARM64_ABI_FPR &&
	 abi.arg[17].reg == LJ_ARM64_ABI_STACK &&
	 abi.arg[17].stack_offset == 4 && abi.arg[17].size == 4);
  assert(trace_has_stack_store(T, A64I_STRw, 2, 0));
  assert(trace_has_stack_store(T, A64I_STRs, 2, 4));
  /* The helper's F32 result must be converted before native leave and the
  ** converted Lua number must be represented in the post-call snapshots. */
  for (ref = trace_nk_acq(T); ref < trace_nins_acq(T); ref++) {
    IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
    if (ins.o == IR_CALLS && ins.op2 == IRCALL_lj_ffi_native_trace_leave)
      leave = ref;
    if (ins.o == IR_CONV && ins.t.irt == IRT_NUM &&
	ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT)) {
      IRIns src = ir_load_acq(&trace_ir_acq(T)[ins.op1]);
      if (src.o == IR_CALLXS) resultconv = ref;
    }
  }
  assert(resultconv != 0 && leave > resultconv);
  {
    MSize s, i;
    int snapshotted = 0;
    for (s = 0; s < trace_nsnap_acq(T); s++) {
      const SnapShot *snap = &trace_snap_acq(T)[s];
      MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
      if (snap_ref_acq(snap) != leave) continue;
      for (i = 0; i < nent; i++)
	if (snap_ref(snapentry_acq(&trace_snapmap_acq(T)[ofs+i])) == resultconv)
	  snapshotted = 1;
    }
    assert(snapshotted);
  }
  for (i = 0; i < abi.nargs; i++) {
    IRIns carg = ir_load_acq(&trace_ir_acq(T)[args[i]]);
    if (i % 2 == 0) assert(irt_type(carg.t) == IRT_INT);
    else assert(irt_type(carg.t) == IRT_FLOAT);
  }
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = bc;
  view.proto = pt;
  view.startpc = trace_startpc_acq(T);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.proto_numparams = pt->numparams;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  view.owner_private = 0;
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);
}

static void test_other_callsite_footer_rejected(lua_State *L)
{
  jit_State *J = G2J(G(L));
  GCfunc *fn;
  GCproto *pt;
  CTState *cts = ctype_ctsG(G(L));
  const BCIns *bc;
  TraceVec *tv;
  GCtrace *T = NULL;
  MSize trace_no, i, s, candidate = UINT_MAX, snapno = UINT_MAX;
  uintptr_t lo, hi;
  LJArm64PostRAView view = { 0 };
  SnapEntry *snapmap;
  IRIns *ircopy;
  uint64_t original, changed;
  IRRef ref;
  SemanticClone semantic;
  SemanticJSave jsave;
  lua_getglobal(L, "__abi_run_three");
  assert(tvisfunc(L->top-1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  bc = proto_bc(pt);
  lo = (uintptr_t)(const void *)bc;
  hi = lo+(uintptr_t)pt->sizebc*sizeof(BCIns);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (trace_no = 1; trace_no < tv->sizetrace; trace_no++) {
    GCtrace *trace = traceref_safe(J, trace_no);
    unsigned calls = 0;
    if (trace == NULL || trace_startpc_acq(trace) == NULL ||
	(uintptr_t)(const void *)trace_startpc_acq(trace) < lo ||
	(uintptr_t)(const void *)trace_startpc_acq(trace) >= hi)
      continue;
    for (ref = trace_nk_acq(trace); ref < trace_nins_acq(trace); ref++)
      if (ir_load_acq(&trace_ir_acq(trace)[ref]).o == IR_CALLXS)
	calls++;
    if (calls >= 3u) {
      T = trace;
      break;
    }
  }
  assert(T != NULL);
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = bc;
  view.proto = pt;
  view.startpc = trace_startpc_acq(T);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.proto_numparams = pt->numparams;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  view.owner_private = 0;
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  snapmap = (SnapEntry *)malloc(view.nsnapmap*sizeof(SnapEntry));
  assert(snapmap != NULL);
  memcpy(snapmap, view.snapmap, view.nsnapmap*sizeof(SnapEntry));
  view.snapmap = snapmap;
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  for (s = 0; s < view.nsnap; s++) {
    const SnapShot *snap = &view.snap[s];
    IRRef at = snap_ref_acq(snap), scan;
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
    SnapEntry footer[1+LJ_FR2];
    uint64_t pcbase;
    int before_leave = 0;
    if (ofs > view.nsnapmap || nent > view.nsnapmap-ofs ||
	view.nsnapmap-ofs-nent < 1u+LJ_FR2)
      continue;
    for (scan = at; scan < view.nins; scan++) {
      IRIns ins = ir_load_acq(&view.ir[scan]);
      if (scan > at && (ins.o == IR_XSAVE || ins.o == IR_CALLXS))
	break;
      if (ins.o == IR_CALLS &&
	  ins.op2 == IRCALL_lj_ffi_native_trace_leave) {
	before_leave = 1;
	break;
      }
    }
    if (!before_leave) continue;
    for (i = 0; i < 1u+LJ_FR2; i++)
      footer[i] = snapentry_acq(&snapmap[ofs+nent+i]);
    memcpy(&pcbase, footer, sizeof(pcbase));
    if ((uintptr_t)(pcbase>>8) < lo || (uintptr_t)(pcbase>>8) >= hi)
      continue;
    snapno = s;
    original = pcbase;
    break;
  }
  assert(snapno != UINT_MAX);
  for (i = 0; i < pt->sizebc; i++) {
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i])) == BC_CALL &&
	lo+(i+1u)*sizeof(BCIns) !=
	(uintptr_t)(original>>8)) {
      candidate = i+1u;
      break;
    }
  }
  assert(candidate != UINT_MAX);
  changed = ((uint64_t)(lo+(uintptr_t)candidate*sizeof(BCIns)) << 8) |
            (original & UINT64_C(0xff));

  /* Prove this source-identity check in the semantic walk independently
  ** from the post-RA mutation below. Each negative starts from a fresh
  ** clone that first passes semantic admission. */
  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  {
    const SnapShot *snap = &semantic.snap[snapno];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
    memcpy(&semantic.snapmap[ofs+nent], &changed, sizeof(changed));
  }
  expect_semantic_reject(J, &semantic.trace,
                         "other valid BC_CALL footer only");
  semantic_clone_free(&semantic);

  semantic_clone_init(&semantic, T);
  (void)semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  {
    IRRef constref;
    MSize s, footer_count = 0;
    unsigned paired = 0;
    for (s = 0; s < semantic.trace.nsnap; s++) {
      const SnapShot *snap = &semantic.snap[s];
      MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
      uint64_t pcbase;
      if (ofs > semantic.trace.nsnapmap ||
          nent > semantic.trace.nsnapmap-ofs ||
          semantic.trace.nsnapmap-ofs-nent < 1u+LJ_FR2)
        continue;
      memcpy(&pcbase, &semantic.snapmap[ofs+nent], sizeof(pcbase));
      if (pcbase == original) {
        memcpy(&semantic.snapmap[ofs+nent], &changed, sizeof(changed));
        footer_count++;
      }
    }
    for (constref = semantic.trace.nk; constref < REF_TRUE; ) {
      IRIns k = ir_load_acq(&semantic.ir[constref]);
      IRRef next = constref+1u;
      if (k.o == IR_KNUM || k.o == IR_KINT64 ||
          (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
                       k.o == IR_KKPTR)))
        next = constref+2u <= REF_TRUE ? constref+2u : REF_DROP;
      if (k.o == IR_KNUM && constref+1u < REF_TRUE &&
          ir_load_acq(&semantic.ir[constref+1u]).tv.u64 == original >> 8) {
        semantic.ir[constref+1u].tv.u64 = changed >> 8;
        paired++;
      }
      assert(next > constref && next <= REF_TRUE);
      constref = next;
    }
    assert(footer_count > 0 && paired > 0);
  }
  expect_semantic_reject(J, &semantic.trace,
                         "other valid BC_CALL footer and paired KNUM");
  semantic_clone_free(&semantic);
  semantic_j_leave(J, &jsave);

  {
    const SnapShot *snap = &view.snap[snapno];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
    memcpy(&((SnapEntry *)view.snapmap)[ofs+nent], &changed, sizeof(changed));
  }
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  {
    IRRef constref;
    int paired = 0, footer_count = 0;
    ircopy = (IRIns *)calloc(view.nins, sizeof(IRIns));
    assert(ircopy != NULL);
    memcpy(&ircopy[view.nk], &trace_ir_acq(T)[view.nk],
	   (view.nins-view.nk)*sizeof(IRIns));
    memcpy(snapmap, trace_snapmap_acq(T), view.nsnapmap*sizeof(SnapEntry));
    view.ir = ircopy;
    assert(lj_asm_arm64_postra_admit(&view, NULL));
    for (s = 0; s < view.nsnap; s++) {
      const SnapShot *snap = &view.snap[s];
      MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
      uint64_t pcbase;
      if (ofs > view.nsnapmap || nent > view.nsnapmap-ofs ||
	  view.nsnapmap-ofs-nent < 1u+LJ_FR2)
	continue;
      memcpy(&pcbase, &((SnapEntry *)view.snapmap)[ofs+nent],
	     sizeof(pcbase));
      if (pcbase == original) {
	memcpy(&((SnapEntry *)view.snapmap)[ofs+nent], &changed,
	       sizeof(changed));
	footer_count++;
      }
    }
    assert(footer_count > 0);
    for (constref = view.nk; constref < REF_TRUE; ) {
      IRIns k = ir_load_acq(&ircopy[constref]);
      IRRef next = constref+1u;
      if (k.o == IR_KNUM || k.o == IR_KINT64 ||
	  (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
		       k.o == IR_KKPTR)))
	next = constref+2u <= REF_TRUE ? constref+2u : REF_DROP;
      assert(next > constref && next <= REF_TRUE);
      if (k.o == IR_KNUM && constref+1u < REF_TRUE &&
	  ir_load_acq(&ircopy[constref+1u]).tv.u64 == original >> 8) {
	ircopy[constref+1u].tv.u64 = (uint64_t)(lo+
	    (uintptr_t)candidate*sizeof(BCIns));
	paired++;
      }
      constref = next;
    }
    assert(paired > 0);
    assert(!lj_asm_arm64_postra_admit(&view, NULL));
    free(ircopy);
  }
  free(snapmap);
}

static void test_callm_admission(lua_State *L)
{
  jit_State *J = G2J(G(L));
  GCfunc *fn;
  GCproto *pt;
  const BCIns *bc;
  TraceVec *tv;
  GCtrace *T = NULL;
  MSize trace_no, source_index = UINT_MAX, i;
  IRRef ref, producer_call = 0, consumer_call = 0, carg_ref;
  IRRef wrong_carg_value = 0;
  uintptr_t lo, hi;
  LJArm64PostRAView view = { 0 };
  SemanticClone semantic;
  SemanticJSave jsave;
  IRIns *copy;
  BCIns producer_original, consumer_original;
  lua_getglobal(L, "__abi_run_callm");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  lua_pop(L, 1);
  bc = proto_bc(pt);
  lo = (uintptr_t)(const void *)bc;
  hi = lo+(uintptr_t)pt->sizebc*sizeof(BCIns);
  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_CALLM) {
      assert(source_index == UINT_MAX);
      source_index = i;
      assert(bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i-1u])) == BC_CALL);
    }
  }
  assert(source_index != UINT_MAX);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (trace_no = 1; trace_no < tv->sizetrace; trace_no++) {
    GCtrace *candidate = traceref_safe(J, trace_no);
    unsigned calls = 0;
    if (candidate == NULL || trace_startpc_acq(candidate) == NULL ||
        (uintptr_t)(const void *)trace_startpc_acq(candidate) < lo ||
        (uintptr_t)(const void *)trace_startpc_acq(candidate) >= hi)
      continue;
    for (ref = trace_nk_acq(candidate); ref < trace_nins_acq(candidate); ref++)
      if (ir_load_acq(&trace_ir_acq(candidate)[ref]).o == IR_CALLXS) calls++;
    if (calls >= 2u) { T = candidate; break; }
  }
  assert(T != NULL);
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = ctype_ctsG(G(L));
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = bc;
  view.proto = pt;
  view.startpc = trace_startpc_acq(T);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.proto_numparams = pt->numparams;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);

  /* Identify the consumer from its exact XSAVE source site and CARG graph. */
  for (ref = view.nk; ref < view.nins; ref++) {
    IRIns ins = ir_load_acq(&view.ir[ref]);
    if (ins.o == IR_XSAVE) {
      BCIns source;
      uint32_t offset = (uint32_t)ins.op1 | ((uint32_t)ins.op2 << 15);
      assert(ins.t.irt == IRT_NIL && ins.op1 < REF_BIAS &&
             ins.op2 < REF_BIAS && offset > 0 && offset < pt->sizebc);
      source = (BCIns)la_load32_acq((const uint32_t *)&bc[offset-1u]);
      if (bc_op(source) == BC_CALLM) {
        IRRef scan;
        for (scan = ref+1u; scan < view.nins; scan++) {
          IRIns next = ir_load_acq(&view.ir[scan]);
          if (next.o == IR_XSAVE) break;
          if (next.o == IR_CALLXS) { consumer_call = scan; break; }
        }
      }
    }
  }
  assert(consumer_call != 0);
  {
    LJArm64ABISignature abi;
    IRRef args[LJ_ARM64_ABI_MAX_ARGS];
    assert(fixture_callxs_signature(view.cts, view.ir, view.nk,
             view.nins, consumer_call, global_ctype(L, "__abi_callm"),
             &abi, args));
    assert(abi.nargs == 2);
    wrong_carg_value = args[0];
    carg_ref = fixture_callxs_carg_for_argument(view.ir, consumer_call,
                                                abi.nargs, 1);
    assert(args[1] == ir_load_acq(&view.ir[carg_ref]).op2);
    producer_call = args[1];
    if (ir_load_acq(&view.ir[producer_call]).o == IR_CONV)
      producer_call = ir_load_acq(&view.ir[producer_call]).op1;
    assert(ir_load_acq(&view.ir[producer_call]).o == IR_CALLXS);
  }

  producer_original = (BCIns)la_load32_acq((const uint32_t *)&bc[source_index-1u]);
  consumer_original = (BCIns)la_load32_acq((const uint32_t *)&bc[source_index]);
  {
    BCIns bads[] = {
      BCINS_ABC(BC_CALL, bc_a(producer_original), 1, bc_c(producer_original)),
      BCINS_ABC(BC_CALL, (BCReg)(bc_a(producer_original)+1u),
                bc_b(producer_original), bc_c(producer_original)),
      BCINS_ABC(BC_CALL, bc_a(producer_original), bc_b(producer_original),
                (BCReg)(bc_c(producer_original)-1u)),
      BCINS_ABC(BC_CALLM, bc_a(consumer_original), bc_b(consumer_original), 0),
      BCINS_ABC(BC_MOV, bc_a(producer_original), 0, 0)
    };
    unsigned mutation;
    for (mutation = 0; mutation < sizeof(bads)/sizeof(bads[0]); mutation++) {
      if (mutation != 3)
        la_store32_rel((uint32_t *)&bc[source_index-1u], bads[mutation]);
      if (mutation == 3)
        la_store32_rel((uint32_t *)&bc[source_index], bads[mutation]);
      semantic_clone_init(&semantic, T);
      jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
      expect_semantic_reject_any(J, &semantic.trace,
                                 "forged CALLM source geometry");
      semantic_j_leave(J, &jsave);
      semantic_clone_free(&semantic);
      assert(!lj_asm_arm64_postra_admit(&view, NULL));
      la_store32_rel((uint32_t *)&bc[source_index-1u], producer_original);
      la_store32_rel((uint32_t *)&bc[source_index], consumer_original);
      assert(lj_asm_arm64_postra_admit(&view, NULL));
    }
  }
  /* Mutating only the consumer's final CARG must fail in each independent
  ** graph walk while retaining the valid source CALL/CALLM pair. */
  semantic_clone_init(&semantic, T);
  jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  expect_semantic_admit(J, &semantic.trace);
  semantic.ir[carg_ref].op2 = wrong_carg_value;
  expect_semantic_reject_any(J, &semantic.trace,
                             "CALLM final CARG not producer result");
  semantic_j_leave(J, &jsave);
  semantic_clone_free(&semantic);
  copy = (IRIns *)calloc(view.nins, sizeof(*copy));
  assert(copy != NULL);
  memcpy(&copy[view.nk], &view.ir[view.nk],
         (view.nins-view.nk)*sizeof(IRIns));
  view.ir = copy;
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  copy[carg_ref].op2 = wrong_carg_value;
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  copy[carg_ref] = ir_load_acq(&trace_ir_acq(T)[carg_ref]);
  {
    const IRType badtypes[] = { IRT_NIL, IRT_CDATA, IRT_P64 };
    unsigned mutation;
    for (mutation = 0; mutation < sizeof(badtypes)/sizeof(badtypes[0]);
         mutation++) {
      semantic_clone_init(&semantic, T);
      jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
      expect_semantic_admit(J, &semantic.trace);
      semantic.ir[producer_call].t.irt = (uint8_t)badtypes[mutation];
      expect_semantic_reject_any(J, &semantic.trace,
                                 "CALLM non-scalar producer result");
      semantic_j_leave(J, &jsave);
      semantic_clone_free(&semantic);
      assert(lj_asm_arm64_postra_admit(&view, NULL));
      copy[producer_call].t.irt = (uint8_t)badtypes[mutation];
      assert(!lj_asm_arm64_postra_admit(&view, NULL));
      copy[producer_call] = ir_load_acq(&trace_ir_acq(T)[producer_call]);
    }
  }
  free(copy);
  view.ir = trace_ir_acq(T);
  {
    SnapEntry *snapmap = (SnapEntry *)malloc(view.nsnapmap*sizeof(SnapEntry));
    MSize s, checked = 0;
    uintptr_t returnpc = lo+(source_index+1u)*sizeof(BCIns);
    uintptr_t producerpc = lo+source_index*sizeof(BCIns);
    assert(snapmap != NULL);
    memcpy(snapmap, view.snapmap, view.nsnapmap*sizeof(SnapEntry));
    view.snapmap = snapmap;
    for (s = 0; s < view.nsnap; s++) {
      const SnapShot *snap = &view.snap[s];
      MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
      uint64_t original, changed;
      IRRef scan, at = snap_ref_acq(snap);
      int before_leave = 0;
      assert(ofs <= view.nsnapmap && nent <= view.nsnapmap-ofs &&
             view.nsnapmap-ofs-nent >= 1u+LJ_FR2);
      memcpy(&original, &snapmap[ofs+nent], sizeof(original));
      if ((uintptr_t)(original>>8) != returnpc) continue;
      for (scan = at; scan < view.nins; scan++) {
        IRIns ins = ir_load_acq(&view.ir[scan]);
        if (scan > at && (ins.o == IR_XSAVE || ins.o == IR_CALLXS)) break;
        if (ins.o == IR_CALLS &&
            ins.op2 == IRCALL_lj_ffi_native_trace_leave) {
          before_leave = 1;
          break;
        }
      }
      if (!before_leave) continue;
      /* Both PCs name valid call continuations, but the earlier one would
      ** replay the consumer after its native call has already completed. */
      changed = ((uint64_t)producerpc << 8) | (original & UINT64_C(0xff));
      semantic_clone_init(&semantic, T);
      jsave = semantic_j_enter(J, pt, &semantic.trace, semantic.ktrace);
      expect_semantic_admit(J, &semantic.trace);
      memcpy(&semantic.snapmap[ofs+nent], &changed, sizeof(changed));
      expect_semantic_reject_any(J, &semantic.trace,
                                 "CALLM leave replays consumer");
      semantic_j_leave(J, &jsave);
      semantic_clone_free(&semantic);
      assert(lj_asm_arm64_postra_admit(&view, NULL));
      memcpy(&snapmap[ofs+nent], &changed, sizeof(changed));
      assert(!lj_asm_arm64_postra_admit(&view, NULL));
      memcpy(&snapmap[ofs+nent], &original, sizeof(original));
      checked++;
    }
    assert(checked > 0);
    free(snapmap);
  }
}

static void test_callm_float_result_layout(lua_State *L)
{
  jit_State *J = G2J(G(L));
  GCproto *pt;
  GCtrace *T = NULL;
  TraceVec *tv = tracevec_acq(J);
  LJArm64PostRAView view;
  IRIns *copy;
  IRRef ref, result = 0;
  MSize n, i;
  uintptr_t lo, hi;
  uint32_t move;
  int found = 0;
  const Reg badregs[] = { RID_D7, RID_D16, RID_X19, RID_NONE };
  lua_getglobal(L, "__abi_run_callm_float_stack");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  lo = (uintptr_t)(const void *)proto_bc(pt);
  hi = lo+pt->sizebc*sizeof(BCIns);
  assert(tv != NULL);
  for (n = 1; n < tv->sizetrace && T == NULL; n++) {
    GCtrace *candidate = traceref_safe(J, n);
    uintptr_t pc;
    if (candidate == NULL) continue;
    pc = (uintptr_t)(const void *)trace_startpc_acq(candidate);
    if (pc >= lo && pc < hi) T = candidate;
  }
  assert(T != NULL);
  view = (LJArm64PostRAView){
    .ir = trace_ir_acq(T), .owner = T, .cts = ctype_ctsG(G(L)),
    .snap = trace_snap_acq(T), .snapmap = trace_snapmap_acq(T),
    .proto_bc = proto_bc(pt), .proto = pt, .startpc = trace_startpc_acq(T),
    .nins = trace_nins_acq(T), .nk = trace_nk_acq(T),
    .nsnap = trace_nsnap_acq(T), .nsnapmap = trace_nsnapmap_acq(T),
    .spadjust = trace_spadjust_acq(T), .proto_sizebc = pt->sizebc,
    .proto_numparams = pt->numparams, .root_topslot = trace_topslot_acq(T),
    .startins = trace_startins_acq(T)
  };
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  for (ref = REF_FIRST; ref < view.nins; ref++) {
    IRIns ins = ir_load_acq(&view.ir[ref]);
    if (ins.o == IR_CALLXS && irt_type(ins.t) == IRT_FLOAT &&
        ins.r >= RID_D8 && ins.r <= RID_D15) {
      result = ref;
      break;
    }
  }
  assert(result != 0);
  move = A64I_FMOV_S |
         A64F_D(ir_load_acq(&view.ir[result]).r & 31u) |
         A64F_N(RID_FPRET & 31u);
  for (i = 0; i < trace_szmcode_acq(T)/sizeof(MCode); i++)
    if ((uint32_t)trace_mcode_acq(T)[i] == move) found = 1;
  assert(found && trace_has_stack_store(T, A64I_STRs, 2u, 0));
  copy = (IRIns *)calloc(view.nins, sizeof(*copy));
  assert(copy != NULL);
  memcpy(&copy[view.nk], &view.ir[view.nk],
         (view.nins-view.nk)*sizeof(*copy));
  view.ir = copy;
  for (i = 0; i < sizeof(badregs)/sizeof(badregs[0]); i++) {
    assert(lj_asm_arm64_postra_admit(&view, NULL));
    copy[result].r = badregs[i];
    assert(!lj_asm_arm64_postra_admit(&view, NULL));
    copy[result] = ir_load_acq(&trace_ir_acq(T)[result]);
  }
  free(copy);
}

static void force_callm_postcall(TGState *tg)
{
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  finish_old_epoch = lj_tg_hs_epoch_ack_acq(tg);
  assert(finish_old_epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch+1u);
  forced_finishes++;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
}

static void observe_native_call(unsigned id)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result;
  const LJFFINativeFrame *frame;
  GCtrace *trace;
  uint32_t flags;
  assert(id > 0 && id < ABI_TEST_MAX_ID);
  result = lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return;  /* Interpreter warm-up calls are counted separately by the library. */
  assert(result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot.depth == 1);
  frame = &snapshot.frame[0];
  flags = lj_ffi_native_frame_flags_acq(frame);
  assert((flags & (LJ_FFI_NATIVE_FRAME_F_ACTIVE |
                   LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
                   LJ_FFI_NATIVE_FRAME_F_POSTCALL)) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((flags & LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_func_acq(frame) == expected_func[id]);
  assert(lj_ffi_native_frame_L_acq(frame) == lj_tg_load_cur_L(test_tg));
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  trace = lj_ffi_native_frame_trace_acq(frame);
  assert(trace != NULL && trace_native_pins_acq(trace) != 0);
  assert(lj_ffi_native_frame_trace_no_acq(frame) != 0);
  assert(lj_tg_in_native_acq(test_tg) == 1);
  assert(lj_tg_load_jit_base(test_tg) != NULL);
  assert(lj_tg_vmstate_load_acq(test_tg) ==
         (int32_t)lj_ffi_native_frame_trace_no_acq(frame));
  native_seen[id]++;
  if (finish_target == id) {
    finish_target = 0;
    lj_ffi_native_trace_test_set_finish_hook(force_callm_postcall);
  }
}

static void test_callm_postcall(lua_State *L, ReadCounter calls,
                               const char *name, const char *inner,
                               const char *outer, unsigned producer,
                               unsigned consumer, unsigned target)
{
  unsigned p0 = native_seen[producer], c0 = native_seen[consumer];
  unsigned pc0 = calls(producer), cc0 = calls(consumer);
  jit_State *J = G2J(G(L));
  GCproto *pt;
  const BCIns *bc;
  GCtrace *T;
  const SnapShot *snap;
  MSize i, source_index = UINT_MAX, ofs, nent;
  uint64_t pcbase;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  bc = proto_bc(pt);
  for (i = 1; i < pt->sizebc; i++) {
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i])) == BC_CALLM) {
      assert(source_index == UINT_MAX);
      source_index = i;
    }
  }
  assert(source_index != UINT_MAX &&
         (target == producer || target == consumer));
  forced_finishes = 0;
  finish_target = target;
  lj_trace_test_reset_exit_stats();
  lua_getglobal(L, name);
  lua_getglobal(L, inner);
  lua_getglobal(L, outer);
  lua_pushinteger(L, 20);
  assert(lua_pcall(L, 3, 1, 0) == 0 && lua_toboolean(L, -1));
  lua_pop(L, 1);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(forced_finishes == 1 && finish_target == 0);
  lj_tg_hs_epoch_ack_rel(test_tg, finish_old_epoch);
  assert(calls(producer) == pc0+20u && calls(consumer) == cc0+20u);
  assert(native_seen[producer] > p0 && native_seen[consumer] > c0);
  assert(lj_trace_test_exit_calls() > 0 && lj_tg_in_native_acq(test_tg) == 0 &&
         lj_tg_load_jit_base(test_tg) == NULL);
  T = traceref_safe(J, lj_trace_test_first_exit_parent());
  assert(T != NULL && trace_native_pins_acq(T) == 0 &&
         lj_trace_test_first_exitno() < trace_nsnap_acq(T));
  snap = &trace_snap_acq(T)[lj_trace_test_first_exitno()];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  assert(ofs <= trace_nsnapmap_acq(T) &&
         nent+1u+LJ_FR2 <= trace_nsnapmap_acq(T)-ofs);
  memcpy(&pcbase, &trace_snapmap_acq(T)[ofs+nent], sizeof(pcbase));
  assert((uintptr_t)(pcbase>>8) ==
         (uintptr_t)(const void *)&bc[source_index+(target == consumer)]);
  printf("CALLM %s forced %s POSTCALL: producer=%u consumer=%u, no replay\n",
         name, target == producer ? "producer" : "consumer",
         native_seen[producer]-p0, native_seen[consumer]-c0);
}

static CTypeID global_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static void assert_abi_class(CTState *cts, CTypeID id, uint8_t result,
                             unsigned nargs, unsigned ngpr, unsigned nfpr)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(cts, id, &abi));
  assert(abi.result_class == result && abi.nargs == nargs);
  assert(abi.ngpr == ngpr && abi.nfpr == nfpr);
}

static void assert_rejected(CTState *cts, CTypeID id)
{
  LJArm64ABISignature abi;
  assert(!lj_ccall_arm64_signature(cts, id, &abi));
}

static void assert_argument_classes(CTState *cts, CTypeID id,
                                    const uint8_t *classes,
                                    const uint8_t *registers,
                                    unsigned nargs)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(cts, id, &abi) && abi.nargs == nargs);
  for (i = 0; i < nargs; i++) {
    assert(abi.arg[i].cls == classes[i]);
    assert(abi.arg[i].reg == registers[i]);
  }
}

static void assert_stack_argument(CTState *cts, CTypeID id, unsigned index,
                                  uint8_t cls, uint16_t offset,
                                  uint8_t size)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(cts, id, &abi));
  assert(index < abi.nargs);
  assert(abi.arg[index].cls == cls);
  assert(abi.arg[index].reg == LJ_ARM64_ABI_STACK);
  assert(abi.arg[index].stack_offset == offset);
  assert(abi.arg[index].size == size);
}

static void assert_stack_extent(CTState *cts, CTypeID id, uint16_t extent)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(cts, id, &abi));
  assert(abi.stack_size == extent);
}

static void test_long_callsite(lua_State *L)
{
	const size_t prefix_count = 33000;
	const char begin[] = "return function(fn) local x=0;";
	const char statement[] = "x=x+1;";
	const char end[] = "for i=1,300 do fn(9) end; return x end";
	size_t capacity = sizeof(begin)+prefix_count*(sizeof(statement)-1u)+
		  sizeof(end);
	char *source = (char *)malloc(capacity);
	char *out;
	size_t i;
	assert(source != NULL);
	out = source;
	memcpy(out, begin, sizeof(begin)-1u);
	out += sizeof(begin)-1u;
	for (i = 0; i < prefix_count; i++) {
		memcpy(out, statement, sizeof(statement)-1u);
		out += sizeof(statement)-1u;
	}
	memcpy(out, end, sizeof(end));
	assert(luaL_loadbuffer(L, source, (size_t)(out-source)+sizeof(end)-1u,
		"=long-callsite") == 0);
	free(source);
	assert(lua_pcall(L, 0, 1, 0) == 0);
	lua_setglobal(L, "__abi_long_callsite");
	ljt_lua_dostring(L,
		"assert(require('jit.util').funcinfo(__abi_long_callsite).bytecodes > 32768)\n"
		"assert(__abi_long_callsite(__abi_unused) == 33000)\n");
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  const char *path = getenv("LJ_ARM64_FFI_ABI_LIB");
  void *library;
  InstallObserver install;
  ReadCounter calls;
  CTState *cts;
  CTypeID six, mixed, many_a, many_b, many_c, unused, f32;
  CTypeID stack9, fpr9, fpr9_float, banks, compact;
  CTypeID aggregate_arg, aggregate_result, vararg;
  unsigned i;
  assert(L != NULL && path != NULL);
  test_tg = L2TG(L);
  library = dlopen(path, RTLD_NOW|RTLD_LOCAL);
  assert(library != NULL);
  install = (InstallObserver)dlsym(library, "arm64_abi_install_observer");
  calls = (ReadCounter)dlsym(library, "arm64_abi_calls");
  assert(install != NULL && calls != NULL);
  expected_func[1] = dlsym(library, "arm64_abi_i32_six");
  expected_func[2] = dlsym(library, "arm64_abi_mixed");
  expected_func[3] = dlsym(library, "arm64_abi_many_a");
  expected_func[4] = dlsym(library, "arm64_abi_many_b");
  expected_func[5] = dlsym(library, "arm64_abi_many_c");
  expected_func[14] = dlsym(library, "arm64_abi_callm_consumer");
  expected_func[6] = dlsym(library, "arm64_abi_unused_result");
  expected_func[7] = dlsym(library, "arm64_abi_float_result");
  expected_func[8] = dlsym(library, "arm64_abi_stack9");
  expected_func[9] = dlsym(library, "arm64_abi_fpr9");
  expected_func[10] = dlsym(library, "arm64_abi_banks_exhausted");
  expected_func[11] = dlsym(library, "arm64_abi_compact_stack");
  expected_func[12] = dlsym(library, "arm64_abi_fpr9_float");
  expected_func[13] = dlsym(library, "arm64_abi_mixed_stack_float");
  for (i = 1; i < ABI_TEST_MAX_ID; i++) assert(expected_func[i] != NULL);
  install(observe_native_call);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int32_t (*abi_six_fn)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);\n"
    "typedef double (*abi_mix_fn)(double,int32_t,double,int32_t,double);\n"
    "typedef int32_t (*abi_pair_fn)(int32_t,int32_t);\n"
    "typedef int32_t (*abi_unused_fn)(int32_t);\n"
    "typedef float (*abi_float_fn)(float,float);\n"
    "typedef int32_t (*abi_stack9_fn)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);\n"
    "typedef double (*abi_fpr9_fn)(double,double,double,double,double,double,double,double,double);\n"
    "typedef float (*abi_fpr9_float_fn)(float,float,float,float,float,float,float,float,float);\n"
    "typedef float (*abi_mixed_stack_float_fn)(int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float);\n"
    "typedef int32_t (*abi_banks_fn)(int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double);\n"
    "typedef int32_t (*abi_compact_fn)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,double,double,double,double,double,double,double,double,int8_t,int16_t,double,void *,int32_t);\n"
    "typedef struct { int64_t a,b; } abi_pair;\n"
    "int32_t arm64_abi_i32_six(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);\n"
    "double arm64_abi_mixed(double,int32_t,double,int32_t,double);\n"
    "int32_t arm64_abi_many_a(int32_t,int32_t);\n"
    "int32_t arm64_abi_many_b(int32_t,int32_t);\n"
    "int32_t arm64_abi_many_c(int32_t,int32_t);\n"
    "int32_t arm64_abi_callm_consumer(int32_t,int32_t);\n"
    "int32_t arm64_abi_unused_result(int32_t);\n"
    "float arm64_abi_float_result(float,float);\n"
    "int32_t arm64_abi_stack9(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);\n"
    "double arm64_abi_fpr9(double,double,double,double,double,double,double,double,double);\n"
    "float arm64_abi_fpr9_float(float,float,float,float,float,float,float,float,float);\n"
    "float arm64_abi_mixed_stack_float(int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float,int32_t,float);\n"
    "int32_t arm64_abi_banks_exhausted(int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double,int32_t,double);\n"
    "int32_t arm64_abi_compact_stack(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,double,double,double,double,double,double,double,double,int8_t,int16_t,double,void *,int32_t);\n"
    "abi_pair arm64_abi_aggregate_arg(abi_pair);\n"
    "abi_pair arm64_abi_aggregate_result(int32_t);\n"
    "int32_t arm64_abi_vararg(int32_t,...);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_ARM64_FFI_ABI_LIB')))\n"
    "_G.__abi_six = lib.arm64_abi_i32_six\n"
    "_G.__abi_mixed = lib.arm64_abi_mixed\n"
    "_G.__abi_a, _G.__abi_b, _G.__abi_c = lib.arm64_abi_many_a, lib.arm64_abi_many_b, lib.arm64_abi_many_c\n"
    "_G.__abi_callm = lib.arm64_abi_callm_consumer\n"
    "_G.__abi_unused = lib.arm64_abi_unused_result\n"
    "_G.__abi_f32 = lib.arm64_abi_float_result\n"
    "_G.__abi_stack9 = lib.arm64_abi_stack9\n"
    "_G.__abi_fpr9 = lib.arm64_abi_fpr9\n"
    "_G.__abi_fpr9_float = lib.arm64_abi_fpr9_float\n"
    "_G.__abi_mixed_f32_stack = lib.arm64_abi_mixed_stack_float\n"
    "_G.__abi_banks = lib.arm64_abi_banks_exhausted\n"
    "_G.__abi_compact = lib.arm64_abi_compact_stack\n"
    "_G.__abi_aggregate_arg = lib.arm64_abi_aggregate_arg\n"
    "_G.__abi_aggregate_result = lib.arm64_abi_aggregate_result\n"
    "_G.__abi_vararg = lib.arm64_abi_vararg\n"
    "function _G.__abi_run_six(fn,a,b,c,d,e,f,n)\n"
    " for i=1,n do if fn(a,b,c,d,e,f)~=91 then return false end end; return true end\n"
    "function _G.__abi_run_mixed(fn,a,b,c,d,e,n)\n"
    " for i=1,n do if fn(a,b,c,d,e)~=16 then return false end end; return true end\n"
    "function _G.__abi_run_mixed_int(fn,a,b,c,d,e,n)\n"
    " for i=1,n do if fn(a,b,c,d,e)~=9 then return false end end; return true end\n"
    "function _G.__abi_run_three(fa,fb,fc,a,b,n)\n"
    " for i=1,n do fa(a,b); fb(a,b); fc(a,b) end; return true end\n"
    "function _G.__abi_run_callm(inner,outer,n)\n"
    " for i=1,n do if outer(7,inner(i,5))~=i+12 then return false end end; return true end\n"
    "function _G.__abi_run_callm_stack(inner,outer,n)\n"
    " for i=1,n do if outer(i,i+1,i+2,i+3,i+4,i+5,i+6,i+7,inner(i,8))~=i+108 then return false end end; return true end\n"
    "function _G.__abi_run_callm_float_double(inner,outer,n)\n"
    " for i=1,n do if outer(1.25,2,3.5,4,inner(5.25,0))~=16 then return false end end; return true end\n"
    "function _G.__abi_run_callm_float_stack(inner,outer,n)\n"
    " for i=1,n do local got=outer(1.1,2.2,3.3,4.4,5.5,6.6,7.7,8.8,inner(9.0,0.9)); if got<9.89 or got>10.1 then return false end end; return true end\n"
    "function _G.__abi_run_callm_double_stack(inner,outer,n)\n"
    " for i=1,n do if outer(1,2,3,4,5,6,7,8,inner(1.25,2,3.5,4,-1.75))~=900 then return false end end; return true end\n"
    "function _G.__abi_run_callm_exit(inner,outer)\n"
    " for i=1,20 do local got=outer(7,inner(i,5)); if got~=i+12 then return false end; if i==4 then return true end end; return false end\n"
    "function _G.__abi_run_unused(fn,a,n)\n"
    " for i=1,n do fn(a) end; return n end\n"
    "function _G.__abi_run_stack_matrix(f9,ffpr,fbanks,fcompact,n)\n"
    " local p = require('ffi').cast('void *',0x12345678)\n"
    " local total=0\n"
    " for k=1,n do\n"
    "  local b=k; local d=b+0.25\n"
    "  local a0,a1,a2,a3,a4,a5,a6,a7=b,b+1,b+2,b+3,b+4,b+5,b+6,b+7\n"
    "  local f0,f1,f2,f3,f4,f5,f6,f7=d,d+1,d+2,d+3,d+4,d+5,d+6,d+7\n"
    "  if f9(a0,a1,a2,a3,a4,a5,a6,a7,b+8) ~= b+108 then return false end\n"
    "  if ffpr(d,d+1,d+2,d+3,d+4,d+5,d+6,d+7,d+8) ~= d*100+800 then return false end\n"
    "  if fbanks(b+9,d+9,b+10,d+10,b+11,d+11,b+12,d+12,b+13,d+13,b+14,d+14,b+15,d+15,b+16,d+16,b+17,d+17) ~= 1010 then return false end\n"
    "  if fcompact(b+19,b+20,b+21,b+22,b+23,b+24,b+25,b+26,d+19,d+20,d+21,d+22,d+23,d+24,d+25,d+26,-7,1234,d+27,p,0x2468) ~= 1111 then return false end\n"
    "  total=total+a0+a1+a2+a3+a4+a5+a6+a7+f0+f1+f2+f3+f4+f5+f6+f7\n"
    " end\n"
    " return total > 0 end\n"
    "function _G.__abi_run_stack_dynamic(fn,n8,n16,p,d0,d1,d2,d3,d4,d5,d6,d7,n)\n"
    " for k=1,n do\n"
    "  if fn(20,21,22,23,24,25,26,27,d0,d1,d2,d3,d4,d5,d6,d7,n8,n16,d7+1,p,0x2468) ~= 1111 then return false end\n"
    " end\n"
    " return true end\n"
    "function _G.__abi_run_float_result(fn,n)\n"
    " for k=1,n do local x=k*0.123456789; local got=fn(x,0.0); if got==x or got<x-0.0001 or got>x+0.0001 then return false end end; return true end\n"
    "function _G.__abi_run_float_range(fn,x,low,high,n)\n"
    " for k=1,n do local got=fn(x*k,0.0); if got<low then return 1 end; if got>high then return 2 end end; return 0 end\n"
    "function _G.__abi_run_float_stack(fn,n)\n"
    " for k=1,n do local x=k*0.000137; local want=x+9.9; local got=fn(x+1.1,x+2.2,x+3.3,x+4.4,x+5.5,x+6.6,x+7.7,x+8.8,x+9.9); if got==want or got<want-0.001 or got>want+0.001 then return false end end; return true end\n"
    "function _G.__abi_run_mixed_float_stack(fn,n)\n"
    " for k=1,n do local x=k*0.000137; local want=x+9.9; local got=fn(k,1.1+x,k+1,2.2+x,k+2,3.3+x,k+3,4.4+x,k+4,5.5+x,k+5,6.6+x,k+6,7.7+x,k+7,8.8+x,k+8,9.9+x); if got==want or got<want-0.001 or got>want+0.001 then return false end end; return true end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  six = global_ctype(L, "__abi_six");
  mixed = global_ctype(L, "__abi_mixed");
  many_a = global_ctype(L, "__abi_a");
  many_b = global_ctype(L, "__abi_b");
  many_c = global_ctype(L, "__abi_c");
  unused = global_ctype(L, "__abi_unused");
  f32 = global_ctype(L, "__abi_f32");
  stack9 = global_ctype(L, "__abi_stack9");
  fpr9 = global_ctype(L, "__abi_fpr9");
  fpr9_float = global_ctype(L, "__abi_fpr9_float");
  banks = global_ctype(L, "__abi_banks");
  compact = global_ctype(L, "__abi_compact");
  aggregate_arg = global_ctype(L, "__abi_aggregate_arg");
  aggregate_result = global_ctype(L, "__abi_aggregate_result");
  vararg = global_ctype(L, "__abi_vararg");

  assert_abi_class(cts, six, LJ_ARM64_ABI_GPR, 6, 6, 0);
  assert_abi_class(cts, mixed, LJ_ARM64_ABI_FPR, 5, 2, 3);
  assert_abi_class(cts, many_a, LJ_ARM64_ABI_GPR, 2, 2, 0);
  assert_abi_class(cts, many_b, LJ_ARM64_ABI_GPR, 2, 2, 0);
  assert_abi_class(cts, many_c, LJ_ARM64_ABI_GPR, 2, 2, 0);
  assert_abi_class(cts, unused, LJ_ARM64_ABI_GPR, 1, 1, 0);
  assert_abi_class(cts, f32, LJ_ARM64_ABI_FPR, 2, 0, 2);
  {
    const uint8_t gpr6[] = { 2, 2, 2, 2, 2, 2 };
    const uint8_t gpr_regs[] = { 0, 1, 2, 3, 4, 5 };
    const uint8_t mixed_classes[] = { 3, 2, 3, 2, 3 };
    const uint8_t mixed_regs[] = { 0, 0, 1, 1, 2 };
    assert_argument_classes(cts, six, gpr6, gpr_regs, 6);
    assert_argument_classes(cts, mixed, mixed_classes, mixed_regs, 5);
  }
  assert_abi_class(cts, stack9, LJ_ARM64_ABI_GPR, 9, 8, 0);
  assert_abi_class(cts, fpr9, LJ_ARM64_ABI_FPR, 9, 0, 8);
  assert_abi_class(cts, banks, LJ_ARM64_ABI_GPR, 18, 8, 8);
  assert_abi_class(cts, compact, LJ_ARM64_ABI_GPR, 21, 8, 8);
  {
    const uint8_t gpr_stack_classes[] = { 2,2,2,2,2,2,2,2,2 };
    const uint8_t gpr_stack_regs[] = { 0,1,2,3,4,5,6,7,LJ_ARM64_ABI_STACK };
    const uint8_t fpr_stack_classes[] = { 3,3,3,3,3,3,3,3,3 };
    const uint8_t fpr_stack_regs[] = { 0,1,2,3,4,5,6,7,LJ_ARM64_ABI_STACK };
    assert_argument_classes(cts, stack9, gpr_stack_classes, gpr_stack_regs, 9);
    assert_argument_classes(cts, fpr9, fpr_stack_classes, fpr_stack_regs, 9);
    assert_stack_argument(cts, stack9, 8, LJ_ARM64_ABI_GPR, 0, 4);
    assert_stack_argument(cts, fpr9, 8, LJ_ARM64_ABI_FPR, 0, 8);
    assert_stack_argument(cts, banks, 16, LJ_ARM64_ABI_GPR, 0, 4);
    assert_stack_argument(cts, banks, 17, LJ_ARM64_ABI_FPR, 8, 8);
    assert_stack_argument(cts, compact, 16, LJ_ARM64_ABI_GPR, 0, 1);
    assert_stack_argument(cts, compact, 17, LJ_ARM64_ABI_GPR, 2, 2);
    assert_stack_argument(cts, compact, 18, LJ_ARM64_ABI_FPR, 8, 8);
    assert_stack_argument(cts, compact, 19, LJ_ARM64_ABI_GPR, 16, 8);
    assert_stack_argument(cts, compact, 20, LJ_ARM64_ABI_GPR, 24, 4);
    assert_stack_extent(cts, stack9, 4);
    assert_stack_extent(cts, fpr9, 8);
    assert_abi_class(cts, fpr9_float, LJ_ARM64_ABI_FPR, 9, 0, 8);
    assert_stack_argument(cts, fpr9_float, 8, LJ_ARM64_ABI_FPR, 0, 4);
    assert_stack_extent(cts, fpr9_float, 4);
    assert_stack_extent(cts, banks, 16);
    assert_stack_extent(cts, compact, 28);
  }
  assert_rejected(cts, aggregate_arg);
  assert_rejected(cts, aggregate_result);
  {
    LJArm64ABISignature vararg_abi;
    assert(lj_ccall_arm64_signature(cts, vararg, &vararg_abi));
    assert(vararg_abi.is_vararg && vararg_abi.nargs == 1 &&
           vararg_abi.result_class == LJ_ARM64_ABI_GPR &&
           vararg_abi.arg[0].cls == LJ_ARM64_ABI_GPR &&
           vararg_abi.arg[0].reg == 0 && vararg_abi.arg[0].size == 4);
  }
  assert_rejected(cts, UINT32_MAX);  /* malformed/out-of-range CType ID */

  {
    unsigned p0 = native_seen[3], c0 = native_seen[14];
    unsigned pc0 = calls(3), cc0 = calls(14);
    ljt_lua_dostring(L, "assert(__abi_run_callm(__abi_a,__abi_callm,300))\n");
    assert(native_seen[3] >= p0+200u && native_seen[14] >= c0+200u);
    assert(calls(3) == pc0+300u && calls(14) == cc0+300u);
  }
  test_callm_postcall(L, calls, "__abi_run_callm", "__abi_a", "__abi_callm",
                     3, 14, 3);
  test_callm_postcall(L, calls, "__abi_run_callm", "__abi_a", "__abi_callm",
                     3, 14, 14);
  {
    unsigned p0 = native_seen[7], c0 = native_seen[2];
    unsigned pc0 = calls(7), cc0 = calls(2);
    ljt_lua_dostring(L,
      "assert(__abi_run_callm_float_double(__abi_f32,__abi_mixed,300))\n");
    assert(native_seen[7] >= p0+200u && native_seen[2] >= c0+200u);
    assert(calls(7) == pc0+300u && calls(2) == cc0+300u);
    printf("CALLM float/double: producer=%u consumer=%u\n",
           native_seen[7]-p0, native_seen[2]-c0);
  }
  {
    unsigned p0 = native_seen[7], c0 = native_seen[12];
    unsigned pc0 = calls(7), cc0 = calls(12);
    ljt_lua_dostring(L,
      "assert(__abi_run_callm_float_stack(__abi_f32,__abi_fpr9_float,300))\n");
    assert(native_seen[7] >= p0+200u && native_seen[12] >= c0+200u);
    assert(calls(7) == pc0+300u && calls(12) == cc0+300u);
    printf("CALLM float/float stack: producer=%u consumer=%u\n",
           native_seen[7]-p0, native_seen[12]-c0);
  }
  test_callm_float_result_layout(L);
  test_callm_postcall(L, calls, "__abi_run_callm_float_stack", "__abi_f32",
                     "__abi_fpr9_float", 7, 12, 7);
  test_callm_postcall(L, calls, "__abi_run_callm_float_stack", "__abi_f32",
                     "__abi_fpr9_float", 7, 12, 12);
  {
    unsigned p0 = native_seen[2], c0 = native_seen[9];
    unsigned pc0 = calls(2), cc0 = calls(9);
    ljt_lua_dostring(L,
      "assert(__abi_run_callm_double_stack(__abi_mixed,__abi_fpr9,300))\n");
    assert(native_seen[2] >= p0+200u && native_seen[9] >= c0+200u);
    assert(calls(2) == pc0+300u && calls(9) == cc0+300u);
    printf("CALLM double/double stack: producer=%u consumer=%u\n",
           native_seen[2]-p0, native_seen[9]-c0);
  }
  test_callm_postcall(L, calls, "__abi_run_callm_double_stack", "__abi_mixed",
                     "__abi_fpr9", 2, 9, 2);
  test_callm_postcall(L, calls, "__abi_run_callm_double_stack", "__abi_mixed",
                     "__abi_fpr9", 2, 9, 9);
  {
    unsigned p0 = native_seen[3], c0 = native_seen[8];
    unsigned pc0 = calls(3), cc0 = calls(8);
    ljt_lua_dostring(L,
      "assert(__abi_run_callm_stack(__abi_a,__abi_stack9,300))\n");
    assert(native_seen[3] >= p0+200u && native_seen[8] >= c0+200u);
    assert(calls(3) == pc0+300u && calls(8) == cc0+300u);
  }
  {
    unsigned p0 = native_seen[3], c0 = native_seen[14];
    unsigned pc0 = calls(3), cc0 = calls(14);
    ljt_lua_dostring(L,
      "assert(__abi_run_callm_exit(__abi_a,__abi_callm))\n");
    assert(native_seen[3] > p0 && native_seen[14] > c0);
    assert(calls(3) == pc0+4u && calls(14) == cc0+4u);
  }
  ljt_lua_dostring(L,
    "assert(__abi_run_six(__abi_six,1,2,3,4,5,6,300))\n"
    "assert(__abi_run_mixed(__abi_mixed,1.25,2,3.5,4,5.25,300))\n"
    "assert(__abi_a(11,5)==16 and __abi_b(11,5)==6 and __abi_c(11,5)==14)\n"
    "assert(__abi_run_three(__abi_a,__abi_b,__abi_c,11,5,300))\n"
    "assert(__abi_run_unused(__abi_unused,9,300)==300)\n");
  assert(native_seen[14] >= 200u && calls(14) >= 300u);
  test_callm_admission(L);
  {
    unsigned id;
    ljt_lua_dostring(L,
      "assert(__abi_run_stack_matrix(__abi_stack9,__abi_fpr9,__abi_banks,__abi_compact,300))\n");
    for (id = 8; id <= 11; id++)
      assert(native_seen[id] >= 200u);
  }
  ljt_lua_dostring(L,
    "assert(__abi_run_float_result(__abi_f32,300))\n"
    "assert(__abi_run_float_stack(__abi_fpr9_float,300))\n"
    "assert(__abi_run_mixed_float_stack(__abi_mixed_f32_stack,300))\n");
  assert(native_seen[7] >= 200u && calls(7) >= 300u);
  assert(native_seen[12] >= 200u && calls(12) >= 300u);
  assert(native_seen[13] >= 200u && calls(13) >= 300u);
  test_float_call_admission(L);
  test_float_stack_emission(L);
  test_mixed_float_stack_emission(L);
  {
    unsigned before = native_seen[11];
    ljt_lua_dostring(L,
      "local ffi=require('ffi'); local p=ffi.cast('void *',0x12345678)\n"
      "assert(__abi_run_stack_dynamic(__abi_compact,-7,1234,p,10.5,11.5,12.5,13.5,14.5,15.5,16.5,17.5,300))\n");
    assert(native_seen[11] >= before+200u);
  }
  {
    unsigned before = native_seen[11];
    ljt_lua_dostring(L,
      "local ffi=require('ffi'); local p=ffi.cast('void *',0x12345678)\n"
      "assert(__abi_run_stack_dynamic(__abi_compact,249,66770,p,10.5,11.5,12.5,13.5,14.5,15.5,16.5,17.5,300))\n");
    assert(native_seen[11] >= before+200u);
  }
  test_stack_call_admission(L);
  {
    unsigned native_before = native_seen[2];
    ljt_lua_dostring(L,
      "assert(__abi_run_mixed_int(__abi_mixed,1,2,1,4,1,300))\n");
    assert(native_seen[2] >= native_before+200u);
  }
	{
	uint64_t before = calls(6);
	uint64_t native_before = native_seen[6];
	test_long_callsite(L);
	assert(calls(6) >= before+300u);
	assert(native_seen[6] >= native_before+200u);
  }
  test_other_callsite_footer_rejected(L);

  {
    unsigned before = native_seen[7];
    unsigned total;
    ljt_lua_dostring(L,
      "assert(__abi_run_float_range(__abi_f32,0.123456789,-100.0,100.0,300)==0)\n");
    assert(native_seen[7] >= before+200u);
    before = native_seen[7];
    total = calls(7);
    ljt_lua_dostring(L,
      "assert(__abi_run_float_range(__abi_f32,0/0,-100.0,100.0,300)==0)\n");
    assert(native_seen[7] >= before+200u && calls(7) == total+300u);
    /* The fourth input overflows F32, after native loop entry. The result
    ** guard exits after the effect, without replaying that fourth call. */
    for (i = 0; i < 2; i++) {
      before = native_seen[7];
      total = calls(7);
      ljt_lua_dostring(L, i == 0 ?
        "assert(__abi_run_float_range(__abi_f32,1e38,-1e39,1e39,300)==2)\n" :
        "assert(__abi_run_float_range(__abi_f32,-1e38,-1e39,1e39,300)==1)\n");
      assert(native_seen[7] >= before+2u && calls(7) == total+4u);
    }
    before = native_seen[7];
    ljt_lua_dostring(L,
      "assert(__abi_run_float_range(__abi_f32,0.123456789,-100.0,100.0,300)==0)\n");
    assert(native_seen[7] >= before+200u);
  }

  for (i = 1; i <= 6; i++) {
    assert(native_seen[i] >= 200);
    assert(calls(i) >= 300);
  }
  assert(native_seen[7] >= 200 && calls(7) >= 300);
  for (i = 8; i < ABI_TEST_MAX_ID; i++) {
    assert(native_seen[i] >= 200);
    assert(calls(i) >= 300);
  }
  install(NULL);
  lua_close(L);
  assert(dlclose(library) == 0);
  printf("t-arm64-jit-ffi-abi-classes OK: register classes, outgoing stack offsets, bank exhaustion and live CALLXS execution checked\n");
  return 0;
}

#else
int main(void)
{
  puts("t-arm64-jit-ffi-abi-classes SKIP: requires experimental Darwin ARM64");
  return 0;
}
#endif
