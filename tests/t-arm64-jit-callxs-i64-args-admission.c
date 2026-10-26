/* Independent semantic and post-RA proof for boxed I64/U64 CALLXS inputs. */

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
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ccall_arm64.h"
#include "lj_ctype.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-i64-args-admission requires ARM64 CALLXS admission"
#endif

typedef struct ArgsFixture {
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins;
  IRRef ktrace;
  IRRef calls[8];
  IRRef xsaves[8];
  unsigned ncalls;
  GCtrace view_owner;
  GCtrace semantic;
} ArgsFixture;

typedef struct JSave {
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
} JSave;

static unsigned semantic_accepts, semantic_rejects;
static unsigned postra_accepts, postra_rejects;

static BCIns loadbc(const BCIns *pc)
{ return (BCIns)la_load32_acq((const uint32_t *)pc); }

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  return pt;
}

static unsigned op_count(const GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned n = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq((GCtrace *)T); ref++)
    if (ir_load_acq(&ir[ref]).o == op) n++;
  return n;
}

static IRRef fixture_kint(const IRIns *ir, IRRef nk, IRRef target,
                          int32_t *valuep)
{
  IRRef ref;
  for (ref = nk; ref < REF_TRUE; ) {
    IRIns k = ir_load_acq(&ir[ref]);
    if (ref == target) {
      if (k.o != IR_KINT || k.t.irt != IRT_INT) return 0;
      if (valuep != NULL) *valuep = k.i;
      return ref;
    }
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR))) {
      if (ref+1u >= REF_TRUE) return 0;
      ref += 2u;
    } else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
               k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL) {
      ref++;
    } else return 0;
  }
  return 0;
}

static IRRef find_ktrace(const GCtrace *T)
{
  IRRef ref, nk = trace_nk_acq((GCtrace *)T);
  const IRIns *ir = trace_ir_acq(T);
  for (ref = nk; ref < REF_TRUE; ) {
    IRIns k = ir_load_acq(&ir[ref]);
    if (k.o == IR_KGC && ir_kgc_load_acq(&ir[ref]) == obj2gco(T)) return ref;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR)))
      ref += 2u;
    else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
             k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL)
      ref++;
    else return 0;
  }
  return 0;
}

static IRRef find_loopref(const IRIns *ir, IRRef nins)
{
  IRRef ref;
  for (ref = REF_FIRST; ref < nins; ref++)
    if (ir_load_acq(&ir[ref]).o == IR_LOOP) return ref;
  return 0;
}

static IRRef semantic_nins(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef nins = trace_nins_acq((GCtrace *)T);
  while (nins > REF_BASE) {
    IRIns tail = ir_load_acq(&ir[nins-1u]);
    if (tail.o == IR_RENAME && tail.t.irt == IRT_NIL &&
        tail.op1 >= REF_FIRST && tail.op1 < nins-1u &&
        tail.op2 < trace_nsnap_acq((GCtrace *)T) && tail.s == SPS_NONE) {
      nins--;
      continue;
    }
    if (tail.o == IR_NOP && tail.t.irt == IRT_NIL &&
        tail.op1 == 0 && tail.op2 == 0 && tail.prev == 0) {
      nins--;
      continue;
    }
    break;
  }
  return nins;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt, unsigned calls)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) == calls)
      return T;
  }
  return NULL;
}

static void init_fixture(lua_State *L, jit_State *J, ArgsFixture *fx,
                         const char *runner, unsigned calls)
{
  IRRef ref;
  fx->pt = global_proto(L, runner);
  fx->owner = find_trace(J, fx->pt, calls);
  if (fx->owner == NULL)
    fprintf(stderr, "CALLXS argument trace missing for %s\n", runner);
  assert(fx->owner != NULL);
  fx->cts = ctype_ctsG(G(L));
  fx->nins = semantic_nins(fx->owner);
  fx->ktrace = find_ktrace(fx->owner);
  assert(fx->cts != NULL && fx->ktrace != 0 &&
         find_loopref(trace_ir_acq(fx->owner), fx->nins) != 0);
  fx->ir = (IRIns *)calloc((size_t)trace_nins_acq(fx->owner)+1u,
                           sizeof(IRIns));
  fx->snap = (SnapShot *)calloc(trace_nsnap_acq(fx->owner), sizeof(SnapShot));
  fx->snapmap = (SnapEntry *)calloc(trace_nsnapmap_acq(fx->owner),
                                    sizeof(SnapEntry));
  assert(fx->ir != NULL && fx->snap != NULL && fx->snapmap != NULL);
  memset(fx->ir, 0, (size_t)trace_nins_acq(fx->owner)*sizeof(IRIns));
  memcpy(&fx->ir[trace_nk_acq(fx->owner)],
         &trace_ir_acq(fx->owner)[trace_nk_acq(fx->owner)],
         (size_t)(trace_nins_acq(fx->owner)-trace_nk_acq(fx->owner))*
           sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)trace_nsnap_acq(fx->owner)*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)trace_nsnapmap_acq(fx->owner)*sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  fx->ncalls = 0;
  for (ref = REF_FIRST; ref < fx->nins; ref++) {
    if (ir_load_acq(&fx->ir[ref]).o == IR_CALLXS) {
      IRRef x;
      assert(fx->ncalls < sizeof(fx->calls)/sizeof(fx->calls[0]));
      fx->calls[fx->ncalls] = ref;
      fx->xsaves[fx->ncalls] = 0;
      for (x = ref; x > REF_FIRST; ) {
        IRRef prev = --x;
        if (ir_load_acq(&fx->ir[prev]).o == IR_XSAVE) {
          fx->xsaves[fx->ncalls] = prev;
          break;
        }
      }
      assert(fx->xsaves[fx->ncalls] != 0);
      fx->ncalls++;
    }
  }
  assert(fx->ncalls == calls);
}

static void reset_fixture(ArgsFixture *fx)
{
  memset(fx->ir, 0, ((size_t)trace_nins_acq(fx->owner)+1u)*sizeof(IRIns));
  memcpy(&fx->ir[trace_nk_acq(fx->owner)],
         &trace_ir_acq(fx->owner)[trace_nk_acq(fx->owner)],
         (size_t)(trace_nins_acq(fx->owner)-trace_nk_acq(fx->owner))*
           sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)trace_nsnap_acq(fx->owner)*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)trace_nsnapmap_acq(fx->owner)*sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
}

static JSave enter_semantic(jit_State *J, ArgsFixture *fx)
{
  JSave save = { J->curfinal, J->pt, J->startpc, J->loopref, J->parent,
    J->exitno, J->baseslot, J->framedepth, J->retdepth, J->ktrace };
  fx->semantic = fx->view_owner;
  fx->semantic.nins = fx->nins;
  fx->ir[fx->ktrace].o = IR_KNUM;
  fx->ir[fx->ktrace].t.irt = IRT_P64;
  fx->ir[fx->ktrace].op12 = 0;
  fx->ir[fx->ktrace].prev = 0;
  fx->ir[fx->ktrace+1u].tv.u64 = 0;
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = trace_startpc_acq(fx->owner);
  J->loopref = find_loopref(fx->ir, fx->nins);
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1u+LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = fx->ktrace;
  return save;
}

static void leave_semantic(jit_State *J, const JSave *save)
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

static LJArm64PostRAView postra_view(ArgsFixture *fx)
{
  LJArm64PostRAView v = { 0 };
  v.ir = fx->ir;
  v.owner = &fx->view_owner;
  v.cts = fx->cts;
  v.snap = fx->snap;
  v.snapmap = fx->snapmap;
  v.proto_bc = proto_bc(fx->pt);
  v.proto = fx->pt;
  v.startpc = trace_startpc_acq(fx->owner);
  v.nins = trace_nins_acq(&fx->view_owner);
  v.nk = trace_nk_acq(fx->owner);
  v.nsnap = trace_nsnap_acq(fx->owner);
  v.nsnapmap = trace_nsnapmap_acq(fx->owner);
  v.spadjust = trace_spadjust_acq(fx->owner);
  v.proto_sizebc = fx->pt->sizebc;
  v.proto_numparams = fx->pt->numparams;
  v.root_topslot = trace_topslot_acq(fx->owner);
  v.startins = trace_startins_acq(fx->owner);
  return v;
}

static void check_semantic(jit_State *J, ArgsFixture *fx, int expected)
{
  LJArm64IRReject reject;
  int got = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (got != expected)
    fprintf(stderr, "I64 args semantic expected %d got %d reason=%d ref=%u op=%u detail=%u\n",
            expected, got, (int)reject.reason, (unsigned)reject.ref,
            (unsigned)reject.op, (unsigned)reject.detail);
  assert(got == expected);
  if (got) semantic_accepts++; else semantic_rejects++;
}

static void check_postra(LJArm64PostRAView *v, int expected)
{
  IRRef semantic = 0;
  int got = lj_asm_arm64_postra_admit(v, &semantic);
  if (got != expected)
    fprintf(stderr, "I64 args post-RA expected %d got %d\n", expected, got);
  assert(got == expected);
  if (got) postra_accepts++; else postra_rejects++;
}

/* Recover the ordered CARG operands without using admission's reader. */
static unsigned carg_values(ArgsFixture *fx, IRRef call, IRRef *values,
                            unsigned cap)
{
  IRRef rev[LJ_ARM64_ABI_MAX_ARGS];
  IRRef ref = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  while (ref >= REF_FIRST && ref < call) {
    IRIns carg = ir_load_acq(&fx->ir[ref]);
    if (carg.o != IR_CARG) break;
    if (carg.t.irt != IRT_NIL || n+1u >= cap ||
        carg.op1 >= ref || carg.op2 >= ref)
      return 0;
    rev[n++] = carg.op2;
    ref = carg.op1;
  }
  if (n == 0) return 0;
  values[0] = ref;
  for (i = 0; i < n; i++) values[i+1u] = rev[n-i-1u];
  return n+1u;
}

static IRRef carg_node_for(ArgsFixture *fx, IRRef call, unsigned arg)
{
  IRRef byarg[LJ_ARM64_ABI_MAX_ARGS];
  IRRef ref = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0;
  while (ref >= REF_FIRST && ref < call) {
    IRIns carg = ir_load_acq(&fx->ir[ref]);
    if (carg.o != IR_CARG) break;
    if (n >= LJ_ARM64_ABI_MAX_ARGS) return 0;
    byarg[n++] = ref;
    ref = carg.op1;
  }
  if (arg > n) return 0;
  return arg <= 1u ? byarg[n-1u] : byarg[n-arg];
}

static void set_carg_value(ArgsFixture *fx, IRRef call, unsigned arg,
                           IRRef value)
{
  IRRef node = carg_node_for(fx, call, arg);
  assert(node != 0);
  if (arg == 0) fx->ir[node].op1 = value;
  else fx->ir[node].op2 = value;
}

static MSize xsave_argument_slot(ArgsFixture *fx, IRRef xsave)
{
  IRIns marker = ir_load_acq(&fx->ir[xsave]);
  uint32_t offset;
  BCIns source;
  assert(marker.o == IR_XSAVE && marker.op1 < REF_BIAS &&
         marker.op2 < REF_BIAS);
  offset = (uint32_t)marker.op1 | ((uint32_t)marker.op2 << 15);
  assert(offset > 0 && offset < fx->pt->sizebc);
  source = loadbc(&proto_bc(fx->pt)[offset-1u]);
  assert(bc_op(source) == BC_CALL || bc_op(source) == BC_CALLM);
  return (MSize)bc_a(source) + 2u*(1u+LJ_FR2) + 1u;
}

static IRRef find_ctype_guard(ArgsFixture *fx, IRRef call, IRRef cdref,
                              IRRef *typeref_out)
{
  IRRef r;
  for (r = REF_FIRST; r < call; r++) {
    IRIns type = ir_load_acq(&fx->ir[r]);
    IRRef g;
    if (type.o != IR_FLOAD || type.op1 != cdref ||
        type.op2 != IRFL_CDATA_CTYPEID)
      continue;
    for (g = r+1u; g < call; g++) {
      IRIns eq = ir_load_acq(&fx->ir[g]);
      IRRef k;
      if (eq.o != IR_EQ || !irt_isguard(eq.t) ||
          (eq.op1 != r && eq.op2 != r)) continue;
      k = eq.op1 == r ? eq.op2 : eq.op1;
      if (fixture_kint(fx->ir, trace_nk_acq(fx->owner), k, NULL)) {
        if (typeref_out != NULL) *typeref_out = r;
        return g;
      }
    }
  }
  return 0;
}

/* Verify the actual ninth-argument ABI and full-width spills, separately
** from accepting the semantic or post-RA clones. */
static void verify_fixed_layout(ArgsFixture *fx)
{
  IRRef call = fx->calls[fx->ncalls-1u], values[LJ_ARM64_ABI_MAX_ARGS];
  IRRef fp = fx->ir[call].op2, typeref, guard, kref;
  LJArm64ABISignature abi;
  unsigned i, spills = 0;
  int32_t ctype;
  assert(fx->ir[fp].o == IR_FLOAD && fx->ir[fp].op2 == IRFL_CDATA_PTR);
  guard = find_ctype_guard(fx, call, fx->ir[fp].op1, &typeref);
  assert(guard != 0);
  kref = fx->ir[guard].op1 == typeref ? fx->ir[guard].op2 : fx->ir[guard].op1;
  assert(fixture_kint(fx->ir, trace_nk_acq(fx->owner), kref, &ctype));
  assert(lj_ccall_arm64_signature(fx->cts, (CTypeID)ctype, &abi));
  assert(abi.nargs == 9 && abi.ngpr == 8 && abi.stack_size == 8);
  assert(carg_values(fx, call, values, LJ_ARM64_ABI_MAX_ARGS) == 9);
  for (i = 0; i < 9; i++) {
    IRIns value = fx->ir[values[i]];
    assert(abi.arg[i].size == 8 && abi.arg[i].cls == LJ_ARM64_ABI_GPR &&
           abi.arg[i].reg == (i < 8 ? i : LJ_ARM64_ABI_STACK));
    assert(value.o == IR_FLOAD && value.op2 == IRFL_CDATA_INT64 &&
           lj_ir_type_size[irt_type(value.t)] == 8);
    if (ra_hasspill(value.s)) { assert((value.s & 1u) == 0); spills++; }
  }
  assert(abi.arg[8].stack_offset == 0 && spills != 0);
}

static IRRef find_source_sload(ArgsFixture *fx, IRRef call, IRRef *payload_out,
                               unsigned argument)
{
  IRRef values[LJ_ARM64_ABI_MAX_ARGS], value;
  IRIns payload;
  assert(carg_values(fx, call, values, LJ_ARM64_ABI_MAX_ARGS) >= argument+1u);
  value = values[argument];
  payload = ir_load_acq(&fx->ir[value]);
  if (payload.o != IR_FLOAD || payload.op2 != IRFL_CDATA_INT64 ||
      payload.op1 < REF_FIRST || payload.op1 >= value ||
      ir_load_acq(&fx->ir[payload.op1]).o != IR_SLOAD)
    return 0;
  *payload_out = value;
  return payload.op1;
}

static int find_snapshot_entry(ArgsFixture *fx, IRRef xsave, MSize slot,
                               MSize *snapindex, MSize *entryindex)
{
  MSize s;
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++) {
    MSize ofs, nent, i;
    if (snap_ref_acq(&fx->snap[s]) != xsave) continue;
    ofs = snap_mapofs_acq(&fx->snap[s]);
    nent = snap_nent_acq(&fx->snap[s]);
    for (i = 0; i < nent; i++) {
      if (snap_slot(fx->snapmap[ofs+i]) == slot) {
        if (snapindex != NULL) *snapindex = s;
        if (entryindex != NULL) *entryindex = ofs+i;
        return 1;
      }
    }
  }
  return 0;
}

/* Swap two genuine IR instructions, remapping every true reference operand
** and snapshot reference/value. The IRIns allocation annotations travel with
** each instruction, so the post-RA clone remains structurally meaningful. */
static void swap_ir_refs(ArgsFixture *fx, IRRef a, IRRef b)
{
  IRIns tmp = fx->ir[a];
  IRRef r;
  MSize s;
  assert(a >= REF_FIRST && b >= REF_FIRST && a < fx->nins && b < fx->nins &&
         a != b);
  fx->ir[a] = fx->ir[b];
  fx->ir[b] = tmp;
  for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
    IRIns *ins = &fx->ir[r];
    uint8_t mode = lj_ir_mode[ins->o];
    if (irm_op1(mode) == IRMref) {
      if (ins->op1 == a) ins->op1 = b;
      else if (ins->op1 == b) ins->op1 = a;
    }
    if (irm_op2(mode) == IRMref) {
      if (ins->op2 == a) ins->op2 = b;
      else if (ins->op2 == b) ins->op2 = a;
    }
  }
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++) {
    SnapShot *snap = &fx->snap[s];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap), i;
    if (snap_ref_acq(snap) == a) snap->ref = b;
    else if (snap_ref_acq(snap) == b) snap->ref = a;
    for (i = 0; i < nent; i++) {
      SnapEntry *entry = &fx->snapmap[ofs+i];
      IRRef ref = snap_ref(*entry);
      if (ref == a) *entry = (*entry & ~(SnapEntry)UINT16_MAX) | b;
      else if (ref == b) *entry = (*entry & ~(SnapEntry)UINT16_MAX) | a;
    }
  }
}

enum Mutation {
  MUT_BAD_CTYPE, MUT_BAD_CTYPE_SOURCE, MUT_INVERT_CTYPE_GUARD,
  MUT_PAYLOAD_BEFORE_GUARD,
  MUT_BAD_PAYLOAD_WIDTH, MUT_BAD_PAYLOAD_SIGNEDNESS,
  MUT_BAD_PAYLOAD_SOURCE, MUT_XLOAD_CARRIER, MUT_PHI_ARGUMENT,
  MUT_SWAPPED_ARGUMENT, MUT_SNAPSHOT_OTHER, MUT_SNAPSHOT_NORESTORE,
  MUT_STACK_WIDTH, MUT_STACK_NORESTORE, MUT_RAW_CONVERSION,
  MUT_BOXED_ARGUMENT_AS_SLOAD, MUT_BOXED_PAYLOAD_SOURCE,
  MUT_BOXED_OTHER_CNEW, MUT_BOXED_SNAPSHOT_OTHER,
  MUT_BOXED_SNAPSHOT_NORESTORE
};

static IRRef other_boxed_i64_load(ArgsFixture *fx, IRRef call,
                                  IRRef current_payload)
{
  IRRef r;
  int32_t current_id, other_id;
  IRRef current_cnew = ir_load_acq(&fx->ir[current_payload]).op1;
  assert(fixture_kint(fx->ir, trace_nk_acq(fx->owner),
                      ir_load_acq(&fx->ir[current_cnew]).op1,
                      &current_id) != 0);
  assert(current_id == CTID_INT64);
  for (r = REF_FIRST; r < call; r++) {
    if (r == current_payload) continue;
    IRIns candidate = ir_load_acq(&fx->ir[r]);
    if (candidate.o == IR_FLOAD && candidate.op2 == IRFL_CDATA_INT64 &&
        candidate.op1 >= REF_FIRST && candidate.op1 < r &&
        ir_load_acq(&fx->ir[candidate.op1]).o == IR_CNEW) {
      assert(fixture_kint(fx->ir, trace_nk_acq(fx->owner),
          ir_load_acq(&fx->ir[candidate.op1]).op1, &other_id) != 0);
      if (other_id == current_id) return r;
    }
  }
  return 0;
}

static void mutate(ArgsFixture *fx, enum Mutation mutation, int pair)
{
  unsigned target = pair ? fx->ncalls-1u : 0u;
  IRRef call = fx->calls[target], values[LJ_ARM64_ABI_MAX_ARGS];
  IRRef payload, source, guard, typeref = 0;
  unsigned n = carg_values(fx, call, values, LJ_ARM64_ABI_MAX_ARGS);
  assert(n >= 9u);
  if (pair && mutation == MUT_BOXED_ARGUMENT_AS_SLOAD) {
    IRRef direct_source, direct_payload;
    direct_source = find_source_sload(fx, call, &direct_payload, 1u);
    assert(direct_source != 0 && direct_payload != 0);
    set_carg_value(fx, call, 0, direct_payload);
    return;
  }
  if (pair && mutation == MUT_BOXED_PAYLOAD_SOURCE) {
    IRRef producer_payload;
    assert(ir_load_acq(&fx->ir[values[0]]).o == IR_FLOAD);
    producer_payload = ir_load_acq(&fx->ir[values[0]]).op1;
    assert(ir_load_acq(&fx->ir[producer_payload]).o == IR_CNEW);
    /* Replacing the exact owned box with another valid same-type source keeps
    ** the payload load well-formed but breaks its CNEW/root provenance. */
    source = find_source_sload(fx, call, &payload, 1u);
    assert(source != 0 && payload != 0);
    fx->ir[values[0]].op1 = source;
    return;
  }
  if (pair && mutation == MUT_BOXED_OTHER_CNEW) {
    IRRef other_box = 0;
    IRRef current_payload = values[0];
    assert(ir_load_acq(&fx->ir[current_payload]).o == IR_FLOAD);
    other_box = other_boxed_i64_load(fx, call, current_payload);
    assert(other_box != 0 && other_box != current_payload &&
           "two-producer trace must contain another same-type boxed input");
    set_carg_value(fx, call, 0, other_box);
    return;
  }
  if (pair && (mutation == MUT_BOXED_SNAPSHOT_OTHER ||
               mutation == MUT_BOXED_SNAPSHOT_NORESTORE)) {
    MSize slot = xsave_argument_slot(fx, fx->xsaves[target]);
    MSize entry;
    assert(find_snapshot_entry(fx, fx->xsaves[target], slot, NULL, &entry));
    if (mutation == MUT_BOXED_SNAPSHOT_NORESTORE) {
      fx->snapmap[entry] |= SNAP_NORESTORE;
    } else {
      IRRef other = other_boxed_i64_load(fx, call, values[0]);
      assert(other != 0);
      fx->snapmap[entry] = (fx->snapmap[entry] & ~(SnapEntry)UINT16_MAX) |
                           ir_load_acq(&fx->ir[other]).op1;
    }
    return;
  }
  source = find_source_sload(fx, call, &payload, 0u);
  assert(source != 0 && payload != 0);
  guard = find_ctype_guard(fx, call, source, &typeref);
  assert(guard != 0 && typeref != 0);
  switch (mutation) {
  case MUT_BAD_CTYPE: {
    IRIns eq = ir_load_acq(&fx->ir[guard]);
    IRRef k = eq.op1 == typeref ? eq.op2 : eq.op1;
    int32_t actual;
    assert(fixture_kint(fx->ir, trace_nk_acq(fx->owner), k, &actual) != 0);
    assert(actual == CTID_INT64 || actual == CTID_UINT64);
    fx->ir[k].i = actual == CTID_INT64 ? CTID_UINT64 : CTID_INT64;
    break;
  }
  case MUT_BAD_CTYPE_SOURCE: {
    IRRef other_payload, other_source = find_source_sload(fx, call,
      &other_payload, 1u);
    assert(other_source != 0 && other_source != source && other_payload != 0);
    fx->ir[typeref].op1 = other_source;
    break;
  }
  case MUT_INVERT_CTYPE_GUARD:
    fx->ir[guard].o = IR_NE;
    break;
  case MUT_PAYLOAD_BEFORE_GUARD:
    assert(guard < payload);
    swap_ir_refs(fx, guard, payload);
    break;
  case MUT_BAD_PAYLOAD_WIDTH:
    fx->ir[payload].t.irt = IRT_INT;
    break;
  case MUT_STACK_WIDTH:
    fx->ir[values[8]].t.irt = IRT_INT;
    break;
  case MUT_STACK_NORESTORE: {
    MSize entry;
    assert(find_snapshot_entry(fx, fx->xsaves[target],
      xsave_argument_slot(fx, fx->xsaves[target])+8u, NULL, &entry));
    fx->snapmap[entry] |= SNAP_NORESTORE;
    break;
  }
  case MUT_RAW_CONVERSION:
    fx->ir[payload].o = IR_CONV;
    fx->ir[payload].op2 = (IRT_I64 << IRCONV_DSH)|IRT_CDATA;
    break;
  case MUT_BAD_PAYLOAD_SIGNEDNESS:
    fx->ir[payload].t.irt = irt_type(fx->ir[payload].t) == IRT_I64 ?
                              IRT_U64 : IRT_I64;
    break;
  case MUT_BAD_PAYLOAD_SOURCE: {
    IRRef other_payload, other_source = find_source_sload(fx, call,
      &other_payload, 1u);
    assert(other_source != 0 && other_source != source && other_payload != 0);
    fx->ir[payload].op1 = other_source;
    break;
  }
  case MUT_XLOAD_CARRIER: {
    IRRef r, ptr = 0;
    for (r = REF_FIRST; r < call; r++) {
      IRIns ins = ir_load_acq(&fx->ir[r]);
      if (ins.o == IR_FLOAD && ins.op2 == IRFL_CDATA_PTR &&
          irt_type(ins.t) == IRT_P64) { ptr = r; break; }
    }
    assert(ptr != 0 && ptr < payload);
    fx->ir[payload].o = IR_XLOAD;
    fx->ir[payload].op1 = ptr;
    fx->ir[payload].op2 = 0;
    break;
  }
  case MUT_PHI_ARGUMENT: {
    IRRef phi_payload = 0, phi_a = 0, phi_b = 0;
    unsigned i, j;
    for (i = 0; i < n; i++) {
      unsigned found = 0;
      for (j = 0; j < n; j++)
        if (values[j] < values[i]) found++;
      if (found >= 2u && values[i] > phi_payload) phi_payload = values[i];
    }
    assert(phi_payload != 0);
    for (i = 0; i < n && phi_b == 0; i++) {
      if (values[i] >= phi_payload) continue;
      if (phi_a == 0) phi_a = values[i]; else phi_b = values[i];
    }
    assert(phi_a != 0 && phi_b != 0 &&
           irt_type(fx->ir[phi_a].t) == irt_type(fx->ir[phi_payload].t) &&
           irt_type(fx->ir[phi_b].t) == irt_type(fx->ir[phi_payload].t));
    fx->ir[phi_payload].o = IR_PHI;
    fx->ir[phi_payload].op1 = phi_a;
    fx->ir[phi_payload].op2 = phi_b;
    break;
  }
  case MUT_SWAPPED_ARGUMENT: {
    IRRef node = carg_node_for(fx, call, 0);
    assert(node != 0 && values[0] != values[1]);
    set_carg_value(fx, call, 0, values[1]);
    break;
  }
  case MUT_SNAPSHOT_OTHER:
  case MUT_SNAPSHOT_NORESTORE: {
    MSize entry;
    assert(find_snapshot_entry(fx, fx->xsaves[target],
                               xsave_argument_slot(fx, fx->xsaves[target]),
                               NULL, &entry));
    if (mutation == MUT_SNAPSHOT_NORESTORE) {
      fx->snapmap[entry] |= SNAP_NORESTORE;
    } else {
      IRRef other_payload, other_source = find_source_sload(fx, call,
        &other_payload, 1u);
      assert(other_source != 0 && other_source != source);
      fx->snapmap[entry] = (fx->snapmap[entry] & ~(SnapEntry)UINT16_MAX) |
                           other_source;
    }
    break;
  }
  case MUT_BOXED_ARGUMENT_AS_SLOAD:
  case MUT_BOXED_PAYLOAD_SOURCE:
  case MUT_BOXED_OTHER_CNEW:
  case MUT_BOXED_SNAPSHOT_OTHER:
  case MUT_BOXED_SNAPSHOT_NORESTORE:
    assert(!"boxed-only mutation reached direct fixture");
    break;
  }
}

static void mutate_and_check_semantic(jit_State *J, ArgsFixture *fx,
                                      enum Mutation mutation, int pair)
{
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize index = (MSize)((start-base)/sizeof(BCIns));
  BCIns saved;
  JSave state;
  assert(index < fx->pt->sizebc);
  saved = loadbc(&bc[index]);
  assert(bc_op(saved) == BC_JFORL);
  bc_publish((const uint32_t *)&bc[index], trace_startins_acq(fx->owner));
  reset_fixture(fx);
  state = enter_semantic(J, fx);
  mutate(fx, mutation, pair);
  check_semantic(J, fx, 0);
  leave_semantic(J, &state);
  bc_publish((const uint32_t *)&bc[index], saved);
}

static void mutate_and_check_postra(ArgsFixture *fx, enum Mutation mutation,
                                    int pair)
{
  LJArm64PostRAView v;
  reset_fixture(fx);
  v = postra_view(fx);
  mutate(fx, mutation, pair);
  check_postra(&v, 0);
}

static void expect_both(ArgsFixture *fx, jit_State *J,
                        enum Mutation mutation, int pair)
{
  mutate_and_check_semantic(J, fx, mutation, pair);
  mutate_and_check_postra(fx, mutation, pair);
}

static void check_original(ArgsFixture *fx, jit_State *J)
{
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize index = (MSize)((start-base)/sizeof(BCIns));
  BCIns saved;
  JSave state;
  LJArm64PostRAView v;
  reset_fixture(fx);
  assert(index < fx->pt->sizebc);
  saved = loadbc(&bc[index]);
  assert(bc_op(saved) == BC_JFORL);
  bc_publish((const uint32_t *)&bc[index], trace_startins_acq(fx->owner));
  state = enter_semantic(J, fx);
  check_semantic(J, fx, 1);
  leave_semantic(J, &state);
  bc_publish((const uint32_t *)&bc[index], saved);
  reset_fixture(fx);
  v = postra_view(fx);
  check_postra(&v, 1);
}

static int run_fixed(lua_State *L, const char *runner, const char *fn,
                     const char *values)
{
  int i, values_index;
  lua_getglobal(L, "__i64args_drive_fixed");
  lua_getglobal(L, runner);
  lua_getglobal(L, fn);
  lua_pushinteger(L, 40);
  lua_getglobal(L, values);
  values_index = lua_gettop(L);
  for (i = 1; i <= 9; i++) lua_rawgeti(L, values_index, i);
  lua_remove(L, 5);
  if (lua_pcall(L, 12, 1, 0) != 0) {
    fprintf(stderr, "I64 argument runner failed: %s\n", lua_tostring(L, -1));
    return 0;
  }
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  return 1;
}

static int run_pair(lua_State *L)
{
  int i, values_index;
  lua_getglobal(L, "__i64args_drive_pair");
  lua_getglobal(L, "__i64args_run_boxed_pair");
  lua_getglobal(L, "__i64args_source");
  lua_getglobal(L, "__i64args_consume");
  lua_pushinteger(L, 40);
  lua_getglobal(L, "__i64args_signed_values");
  values_index = lua_gettop(L);
  for (i = 1; i <= 8; i++) lua_rawgeti(L, values_index, i);
  lua_remove(L, -9);
  if (lua_pcall(L, 12, 1, 0) != 0) {
    fprintf(stderr, "boxed producer/consumer runner failed: %s\n",
            lua_tostring(L, -1));
    return 0;
  }
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  return 1;
}

static int run_pair_two_boxes(lua_State *L)
{
  int i, values_index;
  lua_getglobal(L, "__i64args_drive_pair2");
  lua_getglobal(L, "__i64args_run_boxed_pair2");
  lua_getglobal(L, "__i64args_source");
  lua_getglobal(L, "__i64args_consume2");
  lua_pushinteger(L, 40);
  lua_getglobal(L, "__i64args_signed_values");
  values_index = lua_gettop(L);
  for (i = 1; i <= 7; i++) lua_rawgeti(L, values_index, i);
  lua_remove(L, 6);
  if (lua_pcall(L, 11, 1, 0) != 0) {
    fprintf(stderr, "two-box producer/consumer runner failed: %s\n",
            lua_tostring(L, -1));
    return 0;
  }
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  return 1;
}

static void check_unpublished_callm(jit_State *J, lua_State *L)
{
  GCproto *pt;
  const BCIns *bc;
  MSize i;
  lua_getglobal(L, "__i64args_callm_closed");
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  pt = funcproto(funcV(L->top-1));
  bc = proto_bc(pt);
  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = loadbc(&bc[i]);
    if (bc_op(ins) == BC_CALLM) break;
  }
  assert(i < pt->sizebc && "boxed producer CALLM fixture must contain CALLM");
  lua_pop(L, 1);
  lua_getglobal(L, "__i64args_drive_callm");
  lua_getglobal(L, "__i64args_source");
  lua_getglobal(L, "__i64args_callm_consumer");
  lua_pushinteger(L, 40);
  if (lua_pcall(L, 3, 1, 0) != 0) {
    fprintf(stderr, "boxed CALLM closure runner failed: %s\n",
            lua_tostring(L, -1));
    assert(0);
  }
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  for (TraceNo no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) &&
        trace_startpt_acq(T) == pt)
      assert(op_count(T, IR_CALLXS) == 0);
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  ArgsFixture fx[4] = { { 0 } };
  unsigned i;
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[ typedef int64_t lj_test_i64_alias; typedef uint64_t lj_test_u64_alias;\n"
    "void lj_callxs_i64_args_configure(int32_t);\n"
    "int64_t lj_callxs_i64_args_source(int32_t);\n"
    "int32_t lj_callxs_i64_args_fixed(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_u64_args_fixed(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);\n"
    "int32_t lj_callxs_i64_args_consume(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_i64_args_consume2(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_i64_args_callm_consumer(int32_t,...);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_I64_ARGS_SO')))\n"
    "_G.__i64args_fixed=lib.lj_callxs_i64_args_fixed; _G.__i64args_unsigned=lib.lj_callxs_u64_args_fixed\n"
    "_G.__i64args_source=lib.lj_callxs_i64_args_source; _G.__i64args_consume=lib.lj_callxs_i64_args_consume; _G.__i64args_consume2=lib.lj_callxs_i64_args_consume2; _G.__i64args_callm_consumer=lib.lj_callxs_i64_args_callm_consumer\n"
    "local S={ffi.new('int64_t',0x8000000000000000LL),ffi.new('int64_t',0x7fffffffffffffffLL),ffi.new('int64_t',-1),ffi.new('int64_t',0x0123456789abcdefLL),ffi.new('int64_t',-81985529216486896LL),ffi.new('int64_t',-9223372036854775807LL),ffi.new('int64_t',0x7fffffff),ffi.new('int64_t',0x80000000),ffi.new('int64_t',0x100000000)}\n"
    "local U={ffi.new('uint64_t',0ULL),ffi.new('uint64_t',0xffffffffffffffffULL),ffi.new('uint64_t',0x8000000000000000ULL),ffi.new('uint64_t',0x0123456789abcdefULL),ffi.new('uint64_t',0xfedcba9876543210ULL),ffi.new('uint64_t',0x100000001ULL),ffi.new('uint64_t',0x7fffffffULL),ffi.new('uint64_t',0x80000000ULL),ffi.new('uint64_t',0xffffffffULL)}\n"
    "_G.__i64args_signed_values=S; _G.__i64args_unsigned_values=U\n"
    "function _G.__i64args_run_fixed(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do r=fn(a0,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_unsigned(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do r=fn(a0,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_boxed_pair(source,consume,n,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do local box=source(1); r=consume(box,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_boxed_pair2(source,consume,n,a1,a2,a3,a4,a5,a6,a7) local r; for i=1,n do local box1=source(1); local box2=source(2); r=consume(box1,box2,a1,a2,a3,a4,a5,a6,a7) end; return r end\n"
    "function _G.__i64args_callm_closed(source,consume,n) local r; for i=1,n do r=consume(17,source(1)) end; return r end\n"
    "function _G.__i64args_drive_fixed(run,fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) local got=run(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8); return got end\n"
    "function _G.__i64args_drive_pair(run,source,consume,n,a1,a2,a3,a4,a5,a6,a7,a8) local got=run(source,consume,n,a1,a2,a3,a4,a5,a6,a7,a8); return got end\n"
    "function _G.__i64args_drive_pair2(run,source,consume,n,a1,a2,a3,a4,a5,a6,a7) local got=run(source,consume,n,a1,a2,a3,a4,a5,a6,a7); return got end\n"
    "function _G.__i64args_drive_callm(source,consume,n) local got=__i64args_callm_closed(source,consume,n); return got end\n"
    "local configure=lib.lj_callxs_i64_args_configure; configure(0)\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");

  assert(run_fixed(L, "__i64args_run_fixed", "__i64args_fixed",
                   "__i64args_signed_values"));
  init_fixture(L, J, &fx[0], "__i64args_run_fixed", 2u);
  assert(run_fixed(L, "__i64args_run_unsigned", "__i64args_unsigned",
                   "__i64args_unsigned_values"));
  init_fixture(L, J, &fx[1], "__i64args_run_unsigned", 2u);
  assert(run_pair(L));
  init_fixture(L, J, &fx[2], "__i64args_run_boxed_pair", 4u);
  assert(run_pair_two_boxes(L));
  init_fixture(L, J, &fx[3], "__i64args_run_boxed_pair2", 6u);

  for (i = 0; i < 4; i++) {
    verify_fixed_layout(&fx[i]);
    check_original(&fx[i], J);
  }
  expect_both(&fx[0], J, MUT_BAD_CTYPE, 0);
  expect_both(&fx[0], J, MUT_BAD_CTYPE_SOURCE, 0);
  expect_both(&fx[0], J, MUT_INVERT_CTYPE_GUARD, 0);
  expect_both(&fx[0], J, MUT_PAYLOAD_BEFORE_GUARD, 0);
  expect_both(&fx[0], J, MUT_BAD_PAYLOAD_WIDTH, 0);
  expect_both(&fx[0], J, MUT_BAD_PAYLOAD_SIGNEDNESS, 0);
  expect_both(&fx[0], J, MUT_BAD_PAYLOAD_SOURCE, 0);
  expect_both(&fx[0], J, MUT_XLOAD_CARRIER, 0);
  expect_both(&fx[0], J, MUT_PHI_ARGUMENT, 0);
  expect_both(&fx[0], J, MUT_SWAPPED_ARGUMENT, 0);
  expect_both(&fx[0], J, MUT_STACK_WIDTH, 0);
  expect_both(&fx[0], J, MUT_STACK_NORESTORE, 0);
  expect_both(&fx[0], J, MUT_RAW_CONVERSION, 0);
  expect_both(&fx[0], J, MUT_SNAPSHOT_OTHER, 0);
  expect_both(&fx[0], J, MUT_SNAPSHOT_NORESTORE, 0);
  expect_both(&fx[1], J, MUT_BAD_CTYPE, 0);
  expect_both(&fx[1], J, MUT_BAD_CTYPE_SOURCE, 0);
  expect_both(&fx[1], J, MUT_INVERT_CTYPE_GUARD, 0);
  expect_both(&fx[1], J, MUT_PAYLOAD_BEFORE_GUARD, 0);
  expect_both(&fx[1], J, MUT_BAD_PAYLOAD_WIDTH, 0);
  expect_both(&fx[1], J, MUT_BAD_PAYLOAD_SIGNEDNESS, 0);
  expect_both(&fx[2], J, MUT_BOXED_ARGUMENT_AS_SLOAD, 1);
  expect_both(&fx[2], J, MUT_BOXED_PAYLOAD_SOURCE, 1);
  expect_both(&fx[3], J, MUT_BOXED_OTHER_CNEW, 1);
  expect_both(&fx[3], J, MUT_BOXED_SNAPSHOT_OTHER, 1);
  expect_both(&fx[3], J, MUT_BOXED_SNAPSHOT_NORESTORE, 1);
  check_unpublished_callm(J, L);

  for (i = 0; i < 4; i++) {
    free(fx[i].ir);
    free(fx[i].snap);
    free(fx[i].snapmap);
  }
  lua_close(L);
  printf("CALLXS I64 args semantic=%u+/%u- post-RA=%u+/%u-\n",
         semantic_accepts, semantic_rejects, postra_accepts, postra_rejects);
  puts("t-arm64-jit-callxs-i64-args-admission OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-i64-args-admission SKIP: requires experimental Darwin ARM64");
  return 0;
}

#endif
