/* Darwin ARM64 promoted-scalar variadic CALLXS runtime fixture. */
#include <assert.h>
#include <dlfcn.h>
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
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_ccall_arm64.h"
#include "lj_ctype.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_tg.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"

typedef void (*InstallObserver)(void (*fn)(unsigned));
typedef unsigned (*ReadCounter)(unsigned id);
enum { VA_MAX_ID = 10, VA_ITERS = 24 };

static lua_State *test_L;
static TGState *test_tg;
static void *expected_func[VA_MAX_ID];
static unsigned native_seen[VA_MAX_ID];
static TraceNo observed_trace_no[VA_MAX_ID];
static unsigned finish_target, forced_finishes;
static uint64_t finish_old_epoch;

typedef struct VarargSemanticClone {
  GCtrace trace;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef ktrace;
} VarargSemanticClone;

typedef struct VarargJSave {
  GCtrace *curfinal;
  GCproto *pt;
  const BCIns *startpc;
  IRRef loopref;
  TraceNo parent;
  ExitNo exitno;
  BCReg baseslot;
  int32_t framedepth, retdepth;
  IRRef ktrace;
} VarargJSave;

typedef struct VarargTarget {
  GCtrace *trace;
  GCproto *proto;
  const BCIns *bc;
  const BCIns *sourcepc;
  BCIns sourceins;
  CTypeID ctype;
  LJArm64ABISignature abi;
  IRRef call, xsave;
  IRRef values[LJ_ARM64_ABI_MAX_ARGS];
  IRRef carg_nodes[LJ_ARM64_ABI_MAX_ARGS];
  unsigned actual_count, rawcount;
  MSize snap_index, mapofs, nent, nslots;
  MSize frame_entry, callable_entry, cdata_entry, source_entry;
  IRRef frame_ref, callable_ref, cdata_ref;
  uint32_t base_slot;
  MSize footer_entry;
  uintptr_t returnpc;
} VarargTarget;

typedef enum VarargMutation {
  VA_MUT_MISSING_SEPARATOR,
  VA_MUT_MOVED_SEPARATOR,
  VA_MUT_DUPLICATE_SEPARATOR,
  VA_MUT_WRONG_TAIL_PROMOTION,
  VA_MUT_SWAPPED_SAME_TYPE,
  VA_MUT_WRONG_FIXED_SOURCE,
  VA_MUT_WRONG_ADD_CARRIER_OPERAND,
  VA_MUT_WRONG_FRAME_RETURN,
  VA_MUT_WRONG_FOOTER_PC,
  VA_MUT_WRONG_CDATA_SLOT,
  VA_MUT_NORESTORE_SOURCE,
  VA_MUT_SHORT_STACK
} VarargMutation;

static void vararg_clone_init(VarargSemanticClone *clone, GCtrace *owner)
{
  IRRef nins = trace_nins_acq(owner), nk = trace_nk_acq(owner), ref;
  MSize nsnap = trace_nsnap_acq(owner), nsnapmap = trace_nsnapmap_acq(owner);
  memset(clone, 0, sizeof(*clone));
  clone->ir = (IRIns *)calloc(nins, sizeof(IRIns));
  clone->snap = (SnapShot *)malloc(nsnap*sizeof(SnapShot));
  clone->snapmap = (SnapEntry *)malloc(nsnapmap*sizeof(SnapEntry));
  assert(clone->ir != NULL && clone->snap != NULL &&
         clone->snapmap != NULL);
  memcpy(&clone->ir[nk], &trace_ir_acq(owner)[nk],
         (nins-nk)*sizeof(IRIns));
  memcpy(clone->snap, trace_snap_acq(owner), nsnap*sizeof(SnapShot));
  memcpy(clone->snapmap, trace_snapmap_acq(owner),
         nsnapmap*sizeof(SnapEntry));
  clone->trace = *owner;
  clone->trace.ir = clone->ir;
  clone->trace.snap = clone->snap;
  clone->trace.snapmap = clone->snapmap;
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
  while (clone->trace.nins > REF_FIRST) {
    IRRef tailref = clone->trace.nins-1u;
    IRIns tail = ir_load_acq(&clone->ir[tailref]);
    if (tail.o == IR_RENAME ||
        (tail.o == IR_NOP && tail.t.irt == IRT_NIL &&
         tail.op1 == 0 && tail.op2 == 0 && tail.prev == 0))
      clone->trace.nins = tailref;
    else
      break;
  }
}

static void vararg_clone_free(VarargSemanticClone *clone)
{
  free(clone->snapmap);
  free(clone->snap);
  free(clone->ir);
  memset(clone, 0, sizeof(*clone));
}

static VarargJSave vararg_j_enter(jit_State *J, GCproto *pt, GCtrace *T,
                                  IRRef ktrace)
{
  VarargJSave save = { J->curfinal, J->pt, J->startpc, J->loopref,
    J->parent, J->exitno, J->baseslot, J->framedepth, J->retdepth,
    J->ktrace };
  IRRef ref;
  J->curfinal = NULL;
  J->pt = pt;
  J->startpc = trace_startpc_acq(T);
  J->loopref = 0;
  for (ref = REF_BASE; ref < T->nins; ref++)
    if (T->ir[ref].o == IR_LOOP) { J->loopref = ref; break; }
  assert(J->loopref != 0);
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1+LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = ktrace;
  return save;
}

static void vararg_j_leave(jit_State *J, const VarargJSave *save)
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

static void vararg_semantic_result(jit_State *J, GCtrace *T, int want,
                                   const char *why)
{
  LJArm64IRReject reject;
  uint32_t *startpc = (uint32_t *)trace_startpc_acq(T);
  uint32_t liveins = la_load32_acq(startpc);
  int accepted;
  la_store32_rel(startpc, T->startins);
  accepted = lj_asm_arm64_ir_admit(J, T, &reject);
  la_store32_rel(startpc, liveins);
  if (accepted != want)
    fprintf(stderr, "variadic semantic %s: accepted=%d reason=%d ref=%u\n",
            why, accepted, (int)reject.reason, (unsigned)reject.ref);
  assert(accepted == want);
  if (want) assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);
  else assert(reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

static int vararg_carg_values(const IRIns *ir, IRRef nk, IRRef nins,
                              IRRef call, unsigned count, IRRef *values,
                              IRRef *nodes)
{
  IRRef ref = ir_load_acq(&ir[call]).op1;
  unsigned i;
  if (call < REF_FIRST || call >= nins || count == 0 ||
      count > LJ_ARM64_ABI_MAX_ARGS || values == NULL || nodes == NULL)
    return 0;
  for (i = count; i > 1u; i--) {
    IRIns carg;
    if (ref < REF_FIRST || ref >= call) return 0;
    carg = ir_load_acq(&ir[ref]);
    if (carg.o != IR_CARG || carg.t.irt != IRT_NIL ||
        carg.op1 < nk || carg.op1 >= ref ||
        (carg.op2 != REF_NIL &&
         (carg.op2 < nk || carg.op2 >= ref)))
      return 0;
    nodes[i-1u] = ref;
    values[i-1u] = carg.op2;
    ref = carg.op1;
  }
  if (ref < nk || ref >= call) return 0;
  nodes[0] = 0;
  values[0] = ref;
  return 1;
}

static CTypeID vararg_global_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static int vararg_signature(CTState *cts, const IRIns *ir, IRRef nk,
                            IRRef nins, IRRef call, CTypeID expected,
                            LJArm64ABISignature *abi)
{
  IRIns c, fp, wrapper, wrapperid;
  IRRef fpref, cdref, ref;
  int found = 0;
  if (call < REF_FIRST || call >= nins ||
      (c = ir_load_acq(&ir[call])).o != IR_CALLXS ||
      (fpref = c.op2) < REF_FIRST || fpref >= call)
    return 0;
  fp = ir_load_acq(&ir[fpref]);
  /* Variadic CALLXS records the wrapper CTypeID as a CARG after the
  ** function-pointer load. Keep this independent source check exact. */
  if (fp.o != IR_CARG || fp.t.irt != IRT_NIL ||
      fp.op1 < REF_FIRST || fp.op1 >= fpref ||
      fp.op2 < nk || fp.op2 >= REF_TRUE)
    return 0;
  wrapper = fp;
  wrapperid = ir_load_acq(&ir[wrapper.op2]);
  if (wrapperid.o != IR_KINT || (CTypeID)wrapperid.i != expected)
    return 0;
  fpref = wrapper.op1;
  fp = ir_load_acq(&ir[fpref]);
  if (fp.o != IR_FLOAD || fp.op2 != IRFL_CDATA_PTR ||
      (cdref = fp.op1) < REF_FIRST || cdref >= fpref ||
      ir_load_acq(&ir[cdref]).o != IR_SLOAD)
    return 0;
  for (ref = REF_FIRST; ref < call; ref++) {
    IRIns eq = ir_load_acq(&ir[ref]);
    IRRef idref;
    IRIns k;
    if (eq.o != IR_FLOAD || eq.op1 != cdref ||
        eq.op2 != IRFL_CDATA_CTYPEID)
      continue;
    for (idref = ref+1u; idref < call; idref++) {
      IRIns guard = ir_load_acq(&ir[idref]);
      IRRef kref;
      if (guard.o != IR_EQ || !irt_isguard(guard.t) ||
          (guard.op1 != ref && guard.op2 != ref))
        continue;
      kref = guard.op1 == ref ? guard.op2 : guard.op1;
      if (kref < nk || kref >= REF_TRUE) return 0;
      k = ir_load_acq(&ir[kref]);
      if (k.o != IR_KINT || (CTypeID)k.i != expected) continue;
      if (found || !lj_ccall_arm64_signature(cts, expected, abi)) return 0;
      found = 1;
      break;
    }
  }
  return found;
}

static int vararg_find_target(GCtrace *T, GCproto *pt, CTState *cts,
                              CTypeID expected, VarargTarget *target)
{
  IRRef nins = trace_nins_acq(T), nk = trace_nk_acq(T), call;
  const BCIns *bc = proto_bc(pt);
  memset(target, 0, sizeof(*target));
  for (call = nk; call < nins; call++) {
    LJArm64ABISignature abi;
    IRRef xsave, scan;
    IRIns ins = ir_load_acq(&trace_ir_acq(T)[call]);
    if (ins.o != IR_CALLXS ||
        !vararg_signature(cts, trace_ir_acq(T), nk, nins, call,
                          expected, &abi) || !abi.is_vararg)
      continue;
    xsave = 0;
    for (scan = call; scan > REF_FIRST; ) {
      IRIns prior = ir_load_acq(&trace_ir_acq(T)[--scan]);
      if (prior.o == IR_XSAVE) { xsave = scan; break; }
      if (prior.o == IR_CALLXS) break;
    }
    if (xsave == 0) continue;
    {
      IRIns marker = ir_load_acq(&trace_ir_acq(T)[xsave]);
      uint32_t offset;
      BCIns sourceins;
      uintptr_t returnpc;
      unsigned rawcount;
      if (marker.t.irt != IRT_NIL || marker.op1 >= REF_BIAS ||
          marker.op2 >= REF_BIAS || (marker.op1 == 0 && marker.op2 == 0))
        continue;
      offset = (uint32_t)marker.op1 | ((uint32_t)marker.op2 << 15);
      if (offset == 0 || offset >= pt->sizebc) continue;
      sourceins = (BCIns)la_load32_acq((const uint32_t *)&bc[offset-1u]);
      if (bc_op(sourceins) != BC_CALL || bc_c(sourceins) == 0) continue;
      target->actual_count = (unsigned)bc_c(sourceins)-1u;
      if (target->actual_count <= abi.nargs) continue;
      rawcount = target->actual_count+1u;
      if (rawcount > LJ_ARM64_ABI_MAX_ARGS) continue;
      if (!vararg_carg_values(trace_ir_acq(T), nk, nins, call, rawcount,
                              target->values, target->carg_nodes))
        continue;
      returnpc = (uintptr_t)(const void *)&bc[offset];
      target->trace = T;
      target->proto = pt;
      target->bc = bc;
      target->sourcepc = &bc[offset-1u];
      target->sourceins = sourceins;
      target->ctype = expected;
      target->abi = abi;
      target->call = call;
      target->xsave = xsave;
      target->rawcount = rawcount;
      target->returnpc = returnpc;
      break;
    }
  }
  if (target->call == 0) return 0;
  {
    MSize s;
    MSize base = 1u+LJ_FR2+(MSize)bc_a(target->sourceins)+1u+LJ_FR2;
    if (base > UINT8_MAX || target->trace->snap == NULL ||
        target->trace->snapmap == NULL)
      return 0;
    target->base_slot = (uint32_t)base;
    for (s = 0; s < trace_nsnap_acq(T); s++) {
      const SnapShot *snap = &trace_snap_acq(T)[s];
      MSize ofs, nent, nslots, i;
      if (snap_ref_acq(snap) != target->xsave) continue;
      if (target->snap_index != 0) return 0;
      target->snap_index = s+1u;
      ofs = snap_mapofs_acq(snap);
      nent = snap_nent_acq(snap);
      nslots = snap_nslots_acq(snap);
      if (ofs > trace_nsnapmap_acq(T) || nent > trace_nsnapmap_acq(T)-ofs ||
          trace_nsnapmap_acq(T)-ofs-nent < 1u+LJ_FR2 ||
          base+1u+target->actual_count > nslots)
        return 0;
      target->mapofs = ofs;
      target->nent = nent;
      target->nslots = nslots;
      target->footer_entry = ofs+nent;
      target->frame_entry = target->callable_entry = 0;
      target->cdata_entry = target->source_entry = 0;
      for (i = 0; i < nent; i++) {
        SnapEntry entry = snapentry_acq(&trace_snapmap_acq(T)[ofs+i]);
        MSize slot = snap_slot(entry);
        if (slot == base-1u && (entry & 0x00ff0000u) == SNAP_FRAME) {
          target->frame_entry = ofs+i+1u;
          target->frame_ref = snap_ref(entry);
        } else if (slot == base-1u-LJ_FR2 &&
                   (entry & 0x00ff0000u) == 0) {
          target->callable_entry = ofs+i+1u;
          target->callable_ref = snap_ref(entry);
        } else if (slot == base && (entry & 0x00ff0000u) == 0) {
          target->cdata_entry = ofs+i+1u;
          target->cdata_ref = snap_ref(entry);
        } else if (slot == base+1u && (entry & 0x00ff0000u) == 0) {
          target->source_entry = ofs+i+1u;
        }
      }
      if (target->frame_entry == 0 || target->callable_entry == 0 ||
          target->cdata_entry == 0 || target->source_entry == 0)
        return 0;
      return 1;
    }
  }
  return 0;
}

static void vararg_mutate(VarargMutation mutation, IRIns *ir,
                          SnapEntry *snapmap, const VarargTarget *target)
{
  const IRRef *node = target->carg_nodes;
  const IRRef *value = target->values;
  switch (mutation) {
  case VA_MUT_MISSING_SEPARATOR:
    ir[node[target->abi.nargs]].op2 = value[target->abi.nargs+1u];
    break;
  case VA_MUT_MOVED_SEPARATOR:
    ir[node[target->abi.nargs]].op2 = value[target->abi.nargs+1u];
    ir[node[target->abi.nargs+1u]].op2 = REF_NIL;
    break;
  case VA_MUT_DUPLICATE_SEPARATOR:
    ir[node[target->abi.nargs+2u]].op2 = REF_NIL;
    break;
  case VA_MUT_WRONG_TAIL_PROMOTION:
    assert(ir[value[target->abi.nargs+1u]].o == IR_CONV &&
           ir[value[target->abi.nargs+1u]].op2 == IRCONV_NUM_INT);
    ir[node[target->abi.nargs+1u]].op2 =
      ir[value[target->abi.nargs+1u]].op1;
    break;
  case VA_MUT_SWAPPED_SAME_TYPE: {
    IRRef first = target->abi.nargs+1u;
    IRRef second = first+1u;
    IRRef oldfirst = ir[node[first]].op2;
    assert(irt_type(ir_load_acq(&ir[oldfirst]).t) ==
           irt_type(ir_load_acq(&ir[value[second]]).t));
    ir[node[first]].op2 = value[second];
    ir[node[second]].op2 = oldfirst;
    break;
  }
  case VA_MUT_WRONG_FIXED_SOURCE: {
    unsigned last = target->abi.nargs-1u;
    IRRef oldsource = ir[node[last]].op2;
    IRRef wrongsource = value[last-1u];
    IRIns oldins = ir_load_acq(&ir[oldsource]);
    IRIns wrongins = ir_load_acq(&ir[wrongsource]);
    assert(irt_type(oldins.t) == IRT_INT &&
           irt_type(wrongins.t) == IRT_INT &&
           (oldins.op1 != wrongins.op1 || oldins.op2 != wrongins.op2));
    ir[node[last]].op2 = wrongsource;
    break;
  }
  case VA_MUT_WRONG_ADD_CARRIER_OPERAND: {
    IRRef carrier_ref = value[target->abi.nargs-1u];
    IRIns carrier = ir_load_acq(&ir[carrier_ref]);
    IRRef alternate = ir[value[target->abi.nargs-2u]].op2;
    assert(carrier.o == IR_ADD && irt_type(carrier.t) == IRT_INT &&
           !irt_isguard(carrier.t));
    assert(alternate != carrier.op2 && alternate < REF_TRUE &&
           ir[alternate].o == IR_KINT && ir[alternate].t.irt == IRT_INT);
    carrier.op2 = alternate;
    ir[carrier_ref] = carrier;
    break;
  }
  case VA_MUT_WRONG_FRAME_RETURN:
    ir[target->frame_ref+1u].tv.u64 =
      (uint64_t)(target->returnpc-sizeof(BCIns));
    break;
  case VA_MUT_WRONG_FOOTER_PC: {
    uint64_t footer;
    memcpy(&footer, &snapmap[target->footer_entry], sizeof(footer));
    footer = (((footer >> 8)+sizeof(BCIns)) << 8) | (footer & 0xffu);
    memcpy(&snapmap[target->footer_entry], &footer, sizeof(footer));
    break;
  }
  case VA_MUT_WRONG_CDATA_SLOT: {
    MSize entry = target->cdata_entry-1u;
    SnapEntry word = snapmap[entry];
    BCReg wrong = (BCReg)(target->base_slot+1u);
    snapmap[entry] = (word & 0x00ffffffu) | ((SnapEntry)wrong << 24);
    break;
  }
  case VA_MUT_NORESTORE_SOURCE:
    snapmap[target->source_entry-1u] |= SNAP_NORESTORE;
    break;
  case VA_MUT_SHORT_STACK:
    break;
  }
}

static void vararg_expect_mutation(jit_State *J, const LJArm64PostRAView *base,
                                   const VarargTarget *target,
                                   VarargMutation mutation,
                                   const char *name)
{
  VarargSemanticClone semantic;
  VarargJSave save;
  IRIns *ir;
  SnapEntry *snapmap;
  LJArm64PostRAView view = *base;
  MSize nins = base->nins, nk = base->nk;
  vararg_clone_init(&semantic, target->trace);
  save = vararg_j_enter(J, target->proto, &semantic.trace, semantic.ktrace);
  vararg_semantic_result(J, &semantic.trace, 1, "mutation baseline");
  if (mutation == VA_MUT_SHORT_STACK) {
    /* Stack extent is a post-RA property; semantic IR must stay admissible. */
    vararg_semantic_result(J, &semantic.trace, 1, name);
  } else {
    vararg_mutate(mutation, semantic.ir, semantic.snapmap, target);
    vararg_semantic_result(J, &semantic.trace, 0, name);
  }
  vararg_j_leave(J, &save);
  vararg_clone_free(&semantic);

  ir = (IRIns *)calloc(nins, sizeof(IRIns));
  snapmap = (SnapEntry *)malloc(base->nsnapmap*sizeof(SnapEntry));
  assert(ir != NULL && snapmap != NULL);
  memcpy(&ir[nk], &base->ir[nk], (nins-nk)*sizeof(IRIns));
  memcpy(snapmap, base->snapmap, base->nsnapmap*sizeof(SnapEntry));
  view.ir = ir;
  view.snapmap = snapmap;
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  if (mutation == VA_MUT_SHORT_STACK)
    view.spadjust = 0;
  else
    vararg_mutate(mutation, ir, snapmap, target);
  if (lj_asm_arm64_postra_admit(&view, NULL))
    fprintf(stderr, "variadic post-RA mutation accepted: %s\n", name);
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  free(snapmap);
  free(ir);
}

static void test_vararg_admission_clones(lua_State *L)
{
  jit_State *J = G2J(G(L));
  CTState *cts = ctype_ctsG(G(L));
  GCproto *pt;
  GCtrace *T;
  TraceNo traceno = observed_trace_no[2];
  CTypeID ctype = vararg_global_ctype(L, "__va_mixed");
  VarargTarget target;
  LJArm64PostRAView view = { 0 };
  int found;
  lua_getglobal(L, "__va_run_mixed");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  T = traceref_safe(J, traceno);
  assert(T != NULL && trace_startpc_acq(T) != NULL &&
         (uintptr_t)(const void *)trace_startpc_acq(T) >=
           (uintptr_t)(const void *)proto_bc(pt) &&
         (uintptr_t)(const void *)trace_startpc_acq(T) <
           (uintptr_t)(const void *)(proto_bc(pt)+pt->sizebc));
  found = vararg_find_target(T, pt, cts, ctype, &target);
  assert(found && target.abi.is_vararg && target.abi.nargs == 2 &&
         target.actual_count == 8 && target.rawcount == 9 &&
         target.values[target.abi.nargs] == REF_NIL);
  assert(irt_type(ir_load_acq(&trace_ir_acq(T)[target.values[3]]).t) == IRT_NUM &&
         irt_type(ir_load_acq(&trace_ir_acq(T)[target.values[4]]).t) == IRT_NUM);
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = proto_bc(pt);
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
  {
    VarargSemanticClone semantic;
    VarargJSave save;
    vararg_clone_init(&semantic, T);
    save = vararg_j_enter(J, pt, &semantic.trace, semantic.ktrace);
    vararg_semantic_result(J, &semantic.trace, 1, "varargs baseline");
    vararg_j_leave(J, &save);
    vararg_clone_free(&semantic);
  }
  assert(target.frame_ref+1u < view.nins &&
         ir_load_acq(&view.ir[target.frame_ref]).o == IR_KNUM &&
         target.footer_entry+1u < view.nsnapmap &&
         view.spadjust != 0 && target.frame_entry != 0 &&
         target.callable_entry != 0 && target.cdata_entry != 0);
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_MISSING_SEPARATOR, "missing nil separator");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_MOVED_SEPARATOR, "moved nil separator");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_DUPLICATE_SEPARATOR, "duplicate nil separator");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_WRONG_TAIL_PROMOTION, "wrong numeric tail promotion");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_SWAPPED_SAME_TYPE, "swapped same-typed tail sources");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_WRONG_FRAME_RETURN, "wrong frame return PC");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_WRONG_FOOTER_PC, "wrong footer PC");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_WRONG_CDATA_SLOT, "wrong cdata slot");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_NORESTORE_SOURCE, "NORESTORE vararg source");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_SHORT_STACK, "undersized variadic outgoing stack");
}

static void test_fixed_overflow_admission_clones(lua_State *L)
{
  jit_State *J = G2J(G(L));
  CTState *cts = ctype_ctsG(G(L));
  GCproto *pt;
  GCtrace *T;
  TraceNo traceno = observed_trace_no[3];
  CTypeID ctype = vararg_global_ctype(L, "__va_gpr");
  VarargTarget target;
  LJArm64PostRAView view = { 0 };
  VarargSemanticClone semantic;
  VarargJSave save;
  lua_getglobal(L, "__va_run_gpr");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  T = traceref_safe(J, traceno);
  assert(T != NULL && vararg_find_target(T, pt, cts, ctype, &target));
  assert(target.abi.is_vararg && target.abi.nargs == 9 &&
         target.actual_count == 13 && target.rawcount == 14 &&
         target.values[9] == REF_NIL);
  assert(ir_load_acq(&trace_ir_acq(T)[target.values[8]]).o == IR_ADD &&
         irt_type(ir_load_acq(&trace_ir_acq(T)[target.values[8]]).t) == IRT_INT);
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = proto_bc(pt);
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
  vararg_clone_init(&semantic, T);
  save = vararg_j_enter(J, pt, &semantic.trace, semantic.ktrace);
  vararg_semantic_result(J, &semantic.trace, 1, "fixed overflow baseline");
  vararg_j_leave(J, &save);
  vararg_clone_free(&semantic);
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_WRONG_FIXED_SOURCE,
                         "wrong named overflow integer source");
  vararg_expect_mutation(J, &view, &target,
                         VA_MUT_WRONG_ADD_CARRIER_OPERAND,
                         "changed unchecked ADD carrier operand");
}

static void force_varargs_postcall(TGState *tg)
{
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  finish_old_epoch = lj_tg_hs_epoch_ack_acq(tg);
  assert(finish_old_epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch+1u);
  forced_finishes++;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
}

static void observe_varargs_call(unsigned id)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result;
  const LJFFINativeFrame *frame;
  GCtrace *trace;
  uint32_t flags;
  assert(id > 0 && id < VA_MAX_ID);
  result = lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return;  /* Interpreter calls are still included in the library counter. */
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
  observed_trace_no[id] =
    lj_ffi_native_frame_trace_no_acq(frame);
  assert(lj_tg_in_native_acq(test_tg) == 1);
  assert(lj_tg_load_jit_base(test_tg) != NULL);
  assert(lj_tg_vmstate_load_acq(test_tg) ==
         (int32_t)lj_ffi_native_frame_trace_no_acq(frame));
  native_seen[id]++;
  if (finish_target == id) {
    finish_target = 0;
    lj_ffi_native_trace_test_set_finish_hook(force_varargs_postcall);
  }
}

static void run_case(lua_State *L, const char *name, int n)
{
  lua_getglobal(L, "jit");
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, "flush");
  assert(lua_isfunction(L, -1));
  lua_remove(L, -2);
  ljt_lua_pcall(L, 0, 0, "jit.flush before varargs case");
  /* Enter the runner from a non-tail Lua call so the recorder sees its
  ** real Lua caller frame rather than the C fixture's pcall frame. */
  lua_getglobal(L, "__va_dispatch");
  assert(lua_isfunction(L, -1));
  lua_pushstring(L, name);
  lua_pushinteger(L, n);
  ljt_lua_pcall(L, 2, 1, name);
  assert(lua_toboolean(L, -1));
  lua_pop(L, 1);
}

static unsigned require_native(ReadCounter calls, unsigned id,
                               const char *runner)
{
  unsigned before_calls = calls(id);
  unsigned before_native = native_seen[id];
  run_case(test_L, runner, VA_ITERS);
  assert(calls(id) == before_calls+VA_ITERS);
  assert(native_seen[id] > before_native);
  assert(observed_trace_no[id] != 0 &&
         traceref_safe(G2J(G(test_L)), observed_trace_no[id]) != NULL);
  return calls(id);
}

static const BCIns *find_callm_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  const BCIns *bc;
  MSize i;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  bc = proto_bc(pt);
  for (i = 1; i < pt->sizebc; i++) {
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i])) == BC_CALLM) {
      assert(i > 1);
      assert(bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i-1])) ==
             BC_CALL);
      assert(bc_b((BCIns)la_load32_acq((const uint32_t *)&bc[i-1])) == 0);
      assert(bc_c((BCIns)la_load32_acq((const uint32_t *)&bc[i-1])) == 2);
      lua_pop(L, 1);
      return &bc[i];
    }
  }
  lua_pop(L, 1);
  return NULL;
}

static const BCIns *find_ordinary_call_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  const BCIns *bc;
  MSize i;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  bc = proto_bc(pt);
  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_CALL && bc_b(ins) == 2 && bc_c(ins) == 3) {
      lua_pop(L, 1);
      return &bc[i];
    }
  }
  lua_pop(L, 1);
  return NULL;
}

static void run_callm_postcall(lua_State *L, ReadCounter calls,
                               const BCIns *callm_pc)
{
  unsigned p0 = calls(7), c0 = calls(8);
  unsigned pn0 = native_seen[7], n0 = native_seen[8];
  uint32_t exit_before;
  TraceNo parent;
  ExitNo exitno;
  GCtrace *trace;
  const SnapShot *snap;
  MSize ofs, nent;
  uint64_t pcbase;
  (void)callm_pc;
  forced_finishes = 0;
  finish_target = 8;
  lj_trace_test_reset_exit_stats();
  run_case(L, "__va_run_callm", 20);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_target == 0 && forced_finishes == 1);
  lj_tg_hs_epoch_ack_rel(test_tg, finish_old_epoch);
  assert(calls(7) == p0+20u && calls(8) == c0+20u &&
         native_seen[7] > pn0 && native_seen[8] > n0);
  assert(lj_trace_test_exit_calls() > 0 && lj_tg_in_native_acq(test_tg) == 0 &&
         lj_tg_load_jit_base(test_tg) == NULL);
  exit_before = lj_trace_test_first_exitno();
  parent = lj_trace_test_first_exit_parent();
  exitno = (ExitNo)exit_before;
  trace = traceref_safe(G2J(G(L)), parent);
  assert(trace != NULL && trace_native_pins_acq(trace) == 0 &&
         exitno < trace_nsnap_acq(trace));
  snap = &trace_snap_acq(trace)[exitno];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  assert(ofs <= trace_nsnapmap_acq(trace) &&
         nent+1u+LJ_FR2 <= trace_nsnapmap_acq(trace)-ofs);
  memcpy(&pcbase, &trace_snapmap_acq(trace)[ofs+nent], sizeof(pcbase));
  assert((uintptr_t)(pcbase>>8) ==
         (uintptr_t)(callm_pc+1));
}

static void run_ordinary_postcall(lua_State *L, ReadCounter calls,
                                  const BCIns *call_pc)
{
  unsigned c0 = calls(9), n0 = native_seen[9];
  TraceNo parent;
  ExitNo exitno;
  GCtrace *trace;
  const SnapShot *snap;
  MSize ofs, nent;
  uint64_t pcbase;
  forced_finishes = 0;
  finish_target = 9;
  lj_trace_test_reset_exit_stats();
  run_case(L, "__va_run_ordinary", 20);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_target == 0 && forced_finishes == 1);
  lj_tg_hs_epoch_ack_rel(test_tg, finish_old_epoch);
  assert(calls(9) == c0+20u && native_seen[9] > n0);
  assert(lj_trace_test_exit_calls() > 0 && lj_tg_in_native_acq(test_tg) == 0 &&
         lj_tg_load_jit_base(test_tg) == NULL);
  parent = lj_trace_test_first_exit_parent();
  exitno = lj_trace_test_first_exitno();
  trace = traceref_safe(G2J(G(L)), parent);
  assert(trace != NULL && trace_native_pins_acq(trace) == 0 &&
         exitno < trace_nsnap_acq(trace));
  snap = &trace_snap_acq(trace)[exitno];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  assert(ofs <= trace_nsnapmap_acq(trace) &&
         nent+1u+LJ_FR2 <= trace_nsnapmap_acq(trace)-ofs);
  memcpy(&pcbase, &trace_snapmap_acq(trace)[ofs+nent], sizeof(pcbase));
  assert((uintptr_t)(pcbase>>8) == (uintptr_t)(call_pc+1));
}

int main(int argc, char **argv)
{
  const char *path = getenv("LJ_ARM64_FFI_VARARGS_LIB");
  void *library;
  InstallObserver install;
  ReadCounter calls;
  lua_State *L;
  const BCIns *callm_pc;
  unsigned i;
  (void)argc;
  (void)argv;
  assert(path != NULL && *path != '\0');
  L = ljt_lua_newstate_openlibs();
  assert(L != NULL);
  test_L = L;
  test_tg = L2TG(L);
  library = dlopen(path, RTLD_NOW|RTLD_LOCAL);
  assert(library != NULL);
  install = (InstallObserver)dlsym(library, "arm64_va_install_observer");
  calls = (ReadCounter)dlsym(library, "arm64_va_calls");
  assert(install != NULL && calls != NULL);
  expected_func[1] = dlsym(library, "arm64_va_noextra");
  expected_func[2] = dlsym(library, "arm64_va_mixed");
  expected_func[3] = dlsym(library, "arm64_va_gpr_overflow");
  expected_func[4] = dlsym(library, "arm64_va_fpr_overflow");
  expected_func[5] = dlsym(library, "arm64_va_pressure");
  expected_func[6] = dlsym(library, "arm64_va_rounding");
  expected_func[7] = dlsym(library, "arm64_va_lua_producer");
  expected_func[8] = dlsym(library, "arm64_va_callm_consumer");
  expected_func[9] = dlsym(library, "arm64_va_ordinary_consumer");
  for (i = 1; i < VA_MAX_ID; i++) assert(expected_func[i] != NULL);
  install(observe_varargs_call);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "double arm64_va_noextra(int32_t, ...);\n"
    "double arm64_va_mixed(int32_t, double, ...);\n"
    "int32_t arm64_va_gpr_overflow(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,...);\n"
    "double arm64_va_fpr_overflow(double,double,double,double,double,double,double,double,double,...);\n"
    "double arm64_va_pressure(int32_t, ...);\n"
    "int32_t arm64_va_rounding(int32_t, ...);\n"
    "double arm64_va_lua_producer(int32_t);\n"
    "double arm64_va_callm_consumer(int32_t, ...);\n"
    "double arm64_va_ordinary_consumer(int32_t, ...);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_ARM64_FFI_VARARGS_LIB')))\n"
    "_G.__va_noextra=lib.arm64_va_noextra\n"
    "_G.__va_mixed=lib.arm64_va_mixed\n"
    "_G.__va_gpr=lib.arm64_va_gpr_overflow\n"
    "_G.__va_fpr=lib.arm64_va_fpr_overflow\n"
    "_G.__va_pressure=lib.arm64_va_pressure\n"
    "_G.__va_rounding=lib.arm64_va_rounding\n"
    "_G.__va_producer=lib.arm64_va_lua_producer\n"
    "_G.__va_callm=lib.arm64_va_callm_consumer\n"
    "_G.__va_ordinary=lib.arm64_va_ordinary_consumer\n"
    "function _G.__va_run_noextra(n)\n"
    " local fn=__va_noextra; for i=1,n do if fn(i)~=i+0.25 then return false end end; return true end\n"
    "function _G.__va_run_mixed(n)\n"
    " local fn=__va_mixed; for i=1,n do if fn(i,i+0.125,i+1,i+0.25,true,'mixed',nil,i+0.5)~=5*i+2.875 then return false end end; return true end\n"
    "function _G.__va_run_gpr(n)\n"
    " local fn=__va_gpr; for i=1,n do if fn(i,i+1,i+2,i+3,i+4,i+5,i+6,i+7,i+8,i+9,i+0.5,'gpr-tail',nil)~=i+303 then return false end end; return true end\n"
    "function _G.__va_run_fpr(n)\n"
    " local fn=__va_fpr; for i=1,n do if fn(1,2,3,4,5,6,7,8,9,true,9.5,'fpr-tail',10.25)~=404 then return false end end; return true end\n"
    "function _G.__va_run_pressure(n)\n"
    " local fn=__va_pressure; for i=1,n do if fn(i,i,i+0.25,true,'pressure-a',nil,i+0.5,false,'pressure-b',i+0.75,i+1.0)~=6*i+3.5 then return false end end; return true end\n"
    "function _G.__va_run_rounding(n)\n"
    " local fn=__va_rounding; for i=1,n do if fn(6,-0.0,0.1)~=606 then return false end end; return true end\n"
    "function _G.__va_run_callm(n)\n"
    " local consumer,producer=__va_callm,__va_producer; for i=1,n do if consumer(17,producer(i))~=17+i+0.5 then return false end end; return true end\n"
    "function _G.__va_run_ordinary(n)\n"
    " local fn=__va_ordinary; for i=1,n do if fn(17,i+0.5)~=17+i+0.5 then return false end end; return true end\n"
    "function _G.__va_dispatch(name,n) local got=_G[name](n); return got end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64')\n");

  assert(require_native(calls, 1, "__va_run_noextra") != 0);
  assert(require_native(calls, 2, "__va_run_mixed") != 0);
  test_vararg_admission_clones(L);
  assert(require_native(calls, 3, "__va_run_gpr") != 0);
  test_fixed_overflow_admission_clones(L);
  assert(require_native(calls, 4, "__va_run_fpr") != 0);
  assert(require_native(calls, 5, "__va_run_pressure") != 0);
  assert(require_native(calls, 6, "__va_run_rounding") != 0);
  assert(require_native(calls, 7, "__va_run_callm") != 0);
  callm_pc = find_callm_proto(L, "__va_run_callm");
  assert(callm_pc != NULL);
  run_callm_postcall(L, calls, callm_pc);
  {
    const BCIns *call_pc = find_ordinary_call_proto(L, "__va_run_ordinary");
    assert(call_pc != NULL);
    run_ordinary_postcall(L, calls, call_pc);
  }
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  lua_close(L);
  dlclose(library);
  puts("ARM64 promoted-scalar variadic CALLXS fixtures passed");
  return 0;
}
#else
int main(void)
{
  puts("ARM64 promoted-scalar variadic CALLXS fixture skipped");
  return 0;
}
#endif
