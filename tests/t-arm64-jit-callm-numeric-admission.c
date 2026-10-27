/* Independent semantic and post-RA admission tests for numeric CALLM chains. */

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) &&      \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) &&                                      \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) &&                               \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ccall_arm64.h"
#include "lj_ctype.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "numeric CALLM admission requires ARM64 CALLXS admission"
#endif

enum { CALLM_KINDS = 8, CALLM_FAMILIES = 3, CALLM_RUNS = 24 };

static const char *const family_names[CALLM_FAMILIES] = {"double", "float", "u32"};
static const char *const kind_names[CALLM_KINDS] = {
  "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"
};

typedef struct CallmFixture {
  lua_State *L;
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, ktrace;
  IRRef calls[8], convs[8], xsaves[8], leaves[8];
  unsigned ncalls;
  unsigned family, kind;
  int stack;
  GCtrace view_owner, semantic;
} CallmFixture;

typedef struct JSave {
  GCtrace *curfinal;
  GCproto *pt;
  const BCIns *startpc;
  IRRef loopref;
  TraceNo parent;
  ExitNo exitno;
  BCReg baseslot;
  int32_t framedepth, retdepth;
  IRRef1 ktrace;
} JSave;

static unsigned semantic_accepts, semantic_rejects;
static unsigned postra_accepts, postra_rejects;
static int active_mutation = -1;

static BCIns loadbc(const BCIns *pc) {
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

static GCproto *global_proto(lua_State *L, const char *name) {
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  lua_pop(L, 1);
  return pt;
}

static CTypeID global_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  id = cdataV(L->top - 1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static unsigned op_count(const GCtrace *T, IROp op) {
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned n = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq((GCtrace *)T); ref++)
    if (ir_load_acq(&ir[ref]).o == op)
      n++;
  return n;
}

static IRRef find_ktrace(const GCtrace *T) {
  IRRef ref, nk = trace_nk_acq((GCtrace *)T);
  const IRIns *ir = trace_ir_acq(T);
  for (ref = nk; ref < REF_TRUE;) {
    IRIns k = ir_load_acq(&ir[ref]);
    if (k.o == IR_KGC && ir_kgc_load_acq(&ir[ref]) == obj2gco(T))
      return ref;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR)))
      ref += 2u;
    else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
             k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL)
      ref++;
    else
      return 0;
  }
  return 0;
}

static IRRef semantic_nins(const GCtrace *T) {
  const IRIns *ir = trace_ir_acq(T);
  IRRef nins = trace_nins_acq((GCtrace *)T);
  while (nins > REF_BASE) {
    IRIns tail = ir_load_acq(&ir[nins - 1u]);
    if (tail.o == IR_RENAME && tail.t.irt == IRT_NIL && tail.op1 >= REF_FIRST &&
        tail.op1 < nins - 1u && tail.op2 < trace_nsnap_acq((GCtrace *)T) &&
        tail.s == SPS_NONE) {
      nins--;
      continue;
    }
    if (tail.o == IR_NOP && tail.t.irt == IRT_NIL && !tail.op1 && !tail.op2 &&
        !tail.prev) {
      nins--;
      continue;
    }
    break;
  }
  return nins;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt, unsigned calls) {
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) == calls)
      return T;
  }
  return NULL;
}

static IRRef find_numeric_result(CallmFixture *fx, IRRef call)
{
  IRIns result = ir_load_acq(&fx->ir[call]);
  IRRef r;
  if (irt_type(result.t) == IRT_NUM) return call;
  if (irt_type(result.t) != IRT_FLOAT && irt_type(result.t) != IRT_U32)
    return 0;
  for (r = call + 1u; r < trace_nins_acq(fx->owner); r++) {
    IRIns conv = ir_load_acq(&fx->ir[r]);
    if (conv.o == IR_CONV && conv.op1 == call && conv.t.irt == IRT_NUM &&
        conv.op2 == ((IRT_NUM << IRCONV_DSH) | irt_type(result.t)))
      return r;
    if (conv.o == IR_CALLS && conv.op2 == IRCALL_lj_ffi_native_trace_leave)
      break;
  }
  return 0;
}

static void locate_calls(CallmFixture *fx) {
  IRRef ref;
  fx->ncalls = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(fx->owner); ref++) {
    IRIns call = ir_load_acq(&fx->ir[ref]);
    IRRef r;
    if (call.o != IR_CALLXS)
      continue;
    assert(fx->ncalls < sizeof(fx->calls) / sizeof(fx->calls[0]));
    fx->calls[fx->ncalls] = ref;
    fx->convs[fx->ncalls] = 0;
    fx->xsaves[fx->ncalls] = 0;
    fx->leaves[fx->ncalls] = 0;
    fx->convs[fx->ncalls] = find_numeric_result(fx, ref);
    assert(fx->convs[fx->ncalls] != 0);
    for (r = ref; r > REF_FIRST;) {
      IRRef prev = --r;
      if (ir_load_acq(&fx->ir[prev]).o == IR_XSAVE) {
        fx->xsaves[fx->ncalls] = prev;
        break;
      }
    }
    for (r = ref + 1u; r < trace_nins_acq(fx->owner); r++) {
      IRIns next = ir_load_acq(&fx->ir[r]);
      if (next.o == IR_CALLS && next.op2 == IRCALL_lj_ffi_native_trace_leave) {
        fx->leaves[fx->ncalls] = r;
        break;
      }
    }
    assert(fx->xsaves[fx->ncalls] && fx->leaves[fx->ncalls]);
    fx->ncalls++;
  }
}

static void init_fixture(lua_State *L, jit_State *J, CallmFixture *fx,
                         const char *runner, unsigned family, unsigned kind,
                         int stack, unsigned calls) {
  fx->L = L;
  fx->family = family;
  fx->kind = kind;
  fx->stack = stack;
  fx->pt = global_proto(L, runner);
  fx->owner = find_trace(J, fx->pt, calls);
  if (!fx->owner)
    fprintf(stderr, "numeric CALLM trace missing for %s\n", runner);
  assert(fx->owner != NULL);
  fx->cts = ctype_ctsG(G(L));
  fx->nins = semantic_nins(fx->owner);
  fx->ktrace = find_ktrace(fx->owner);
  assert(fx->cts && fx->ktrace);
  fx->ir =
      (IRIns *)calloc((size_t)trace_nins_acq(fx->owner) + 1u, sizeof(IRIns));
  fx->snap = (SnapShot *)calloc(trace_nsnap_acq(fx->owner), sizeof(SnapShot));
  fx->snapmap =
      (SnapEntry *)calloc(trace_nsnapmap_acq(fx->owner), sizeof(SnapEntry));
  assert(fx->ir && fx->snap && fx->snapmap);
  memcpy(&fx->ir[trace_nk_acq(fx->owner)],
         &trace_ir_acq(fx->owner)[trace_nk_acq(fx->owner)],
         (size_t)(trace_nins_acq(fx->owner) - trace_nk_acq(fx->owner)) *
             sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)trace_nsnap_acq(fx->owner) * sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)trace_nsnapmap_acq(fx->owner) * sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  locate_calls(fx);
  assert(fx->ncalls == calls);
}

static void reset_fixture(CallmFixture *fx) {
  memcpy(&fx->ir[trace_nk_acq(fx->owner)],
         &trace_ir_acq(fx->owner)[trace_nk_acq(fx->owner)],
         (size_t)(trace_nins_acq(fx->owner) - trace_nk_acq(fx->owner)) *
             sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)trace_nsnap_acq(fx->owner) * sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)trace_nsnapmap_acq(fx->owner) * sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  locate_calls(fx);
}

static JSave enter_semantic(jit_State *J, CallmFixture *fx) {
  JSave save = {J->curfinal, J->pt,     J->startpc,  J->loopref,
                J->parent,   J->exitno, J->baseslot, J->framedepth,
                J->retdepth, J->ktrace};
  fx->semantic = fx->view_owner;
  fx->semantic.nins = fx->nins;
  fx->ir[fx->ktrace].o = IR_KNUM;
  fx->ir[fx->ktrace].t.irt = IRT_P64;
  fx->ir[fx->ktrace].op12 = 0;
  fx->ir[fx->ktrace].prev = 0;
  fx->ir[fx->ktrace + 1u].tv.u64 = 0;
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = trace_startpc_acq(fx->owner);
  J->loopref = 0;
  for (IRRef r = REF_FIRST; r < fx->nins; r++)
    if (ir_load_acq(&fx->ir[r]).o == IR_LOOP) {
      J->loopref = r;
      break;
    }
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1u + LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = fx->ktrace;
  return save;
}

static void leave_semantic(jit_State *J, const JSave *s) {
  J->curfinal = s->curfinal;
  J->pt = s->pt;
  J->startpc = s->startpc;
  J->loopref = s->loopref;
  J->parent = s->parent;
  J->exitno = s->exitno;
  J->baseslot = s->baseslot;
  J->framedepth = s->framedepth;
  J->retdepth = s->retdepth;
  J->ktrace = s->ktrace;
}

static LJArm64PostRAView postra_view(CallmFixture *fx) {
  LJArm64PostRAView v = {0};
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

static void expect_semantic(jit_State *J, CallmFixture *fx, int accepted) {
  LJArm64IRReject reject;
  int got = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (got != accepted)
    fprintf(
        stderr,
        "numeric CALLM semantic mutation=%d expected %d got %d reason=%d ref=%u op=%u detail=%u\n",
        active_mutation, accepted, got, (int)reject.reason, (unsigned)reject.ref,
        (unsigned)reject.op, (unsigned)reject.detail);
  assert(got == accepted);
  if (accepted)
    semantic_accepts++;
  else
    semantic_rejects++;
}

static void expect_postra(LJArm64PostRAView *v, int accepted) {
  IRRef n = 0;
  int got = lj_asm_arm64_postra_admit(v, &n);
  if (got != accepted)
    fprintf(stderr, "numeric CALLM post-RA mutation=%d expected %d got %d\n",
            active_mutation, accepted, got);
  assert(got == accepted);
  if (accepted) {
    postra_accepts++;
    assert(n > REF_FIRST && n <= v->nins);
  } else
    postra_rejects++;
}

enum Mutation {
  M_CALLM_RAW_TAIL,
  M_CALLM_WRONG_PRODUCER,
  M_CALLM_BAD_CONV_MODE,
  M_CALLM_BAD_CONV_FLAGS,
  M_CALLM_BAD_CONV_SOURCE,
  M_CALLM_BAD_RESULT_MODE,
  M_CALLM_BAD_RESULT_SOURCE,
  M_CALLM_BAD_PRODUCER_TYPE,
  M_CALLM_ABI_SIGN,
  M_CALLM_SOURCE_WRONG,
  M_CALLM_U32_RAW_SNAPSHOT,
  M_CALLM_U32_CARRIER_USE,
  M_CALLM_STACK_U32_CARRIER,
  M_CALLM_STACK_U32_CONSTANT,
  M_CALLM_STACK_U32_SNAPSHOT,
  M_CALLM_STACK_INT_CARRIER,
  M_CALLM_CARRIER_SNAPSHOT,
  M_CALLM_PRODUCER_WRONG_RESULT,
  M_CALLM_PRODUCER_NORESTORE,
  M_CALLM_CONSUMER_NORESTORE,
  M_CALLM_CONSUMER_MISSING,
  M_CALLM_CONSUMER_BAD_PC,
  M_CALLM_CONSUMER_BAD_COUNT,
  M_CALLM_BAD_BANK,
  M_CALLM_BAD_SPILL_ALIGN,
  M_CALLM_BAD_SPILL_EXTENT
};

static void change_slot(CallmFixture *fx, IRRef snapref, BCReg slot,
                        IRRef replacement, int flags, int remove) {
  MSize s;
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++)
    if (snap_ref_acq(&fx->snap[s]) == snapref) {
      MSize ofs = snap_mapofs_acq(&fx->snap[s]),
            n = snap_nent_acq(&fx->snap[s]), i;
      for (i = 0; i < n; i++)
        if (snap_slot(fx->snapmap[ofs + i]) == slot) {
          SnapEntry e = fx->snapmap[ofs + i];
          if (remove)
            fx->snapmap[ofs + i] = (e & ~((SnapEntry)UINT16_MAX)) | REF_NIL;
          else
            fx->snapmap[ofs + i] =
                (e & ~((SnapEntry)UINT16_MAX)) | replacement | flags;
          return;
        }
    }
  assert(!"snapshot slot not found");
}

static BCIns source_call(CallmFixture *fx, unsigned ix, BCReg *slotp);

static void add_side_use(CallmFixture *fx, IRRef value)
{
  IRRef r;
  IRType type = irt_type(ir_load_acq(&fx->ir[value]).t);
  for (r = value + 1u; r < trace_nins_acq(fx->owner); r++) {
    IRIns ins = ir_load_acq(&fx->ir[r]);
    if (ins.o == IR_ADD && irt_type(ins.t) == type &&
        ins.op1 != value && ins.op2 != value) {
      fx->ir[r].op1 = value;
      return;
    }
  }
  assert(!"numeric CALLM fixture has no later arithmetic use to corrupt");
}

static void carrier_in_unrelated_loop_snapshot(CallmFixture *fx, IRRef carrier,
                                               unsigned consumer)
{
  BCIns src = source_call(fx, consumer, NULL);
  MSize s;
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    MSize ofs = snap_mapofs_acq(&fx->snap[s]);
    MSize nent = snap_nent_acq(&fx->snap[s]), i;
    if (at <= carrier ||
        ofs > trace_nsnapmap_acq(fx->owner) ||
        nent > trace_nsnapmap_acq(fx->owner)-ofs) continue;
    for (i = 0; i < nent; i++) {
      SnapEntry entry = fx->snapmap[ofs+i];
      BCReg slot = snap_slot(entry);
      IRRef old = snap_ref(entry);
      if (slot >= bc_a(src) || entry != SNAP(slot, 0, old) ||
          old < trace_nk_acq(fx->owner) || old >= at ||
          irt_type(ir_load_acq(&fx->ir[old]).t) != IRT_INT) continue;
      fx->snapmap[ofs+i] = SNAP(slot, 0, carrier);
      return;
    }
  }
  assert(!"no ordinary loop INT snapshot slot for narrow carrier negative");
}

static BCIns source_call(CallmFixture *fx, unsigned ix, BCReg *slotp) {
  MSize s;
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++)
    if (snap_ref_acq(&fx->snap[s]) == fx->xsaves[ix]) {
      IRIns marker = ir_load_acq(&fx->ir[fx->xsaves[ix]]);
      uint32_t offset = (uint32_t)marker.op1 | ((uint32_t)marker.op2 << 15);
      BCIns ins;
      assert(marker.o == IR_XSAVE && offset > 0 && offset < fx->pt->sizebc);
      ins = loadbc(&proto_bc(fx->pt)[offset - 1u]);
      assert(bc_op(ins) == BC_CALL || bc_op(ins) == BC_CALLM);
      if (slotp)
        *slotp = (BCReg)(1u + LJ_FR2 + bc_a(ins));
      return ins;
    }
  assert(!"missing exact XSAVE snapshot");
  return (BCIns)0;
}

static const BCIns *source_returnpc(CallmFixture *fx, unsigned ix) {
  IRIns marker = ir_load_acq(&fx->ir[fx->xsaves[ix]]);
  uint32_t offset = (uint32_t)marker.op1 | ((uint32_t)marker.op2 << 15);
  assert(marker.o == IR_XSAVE && offset > 0 && offset < fx->pt->sizebc);
  return &proto_bc(fx->pt)[offset];
}

static void assert_result_continuation(CallmFixture *fx, unsigned ix) {
  BCIns ins = source_call(fx, ix, NULL);
  MSize s;
  (void)ins;
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++) {
    if (snap_ref_acq(&fx->snap[s]) == fx->leaves[ix]) {
      MSize ofs = snap_mapofs_acq(&fx->snap[s]);
      MSize n = snap_nent_acq(&fx->snap[s]);
      assert(snap_count_acq(&fx->snap[s]) == SNAPCOUNT_DONE);
      assert(snap_pc(&fx->snapmap[ofs + n]) == source_returnpc(fx, ix));
      return;
    }
  }
  assert(!"numeric CALLXS lacks exact post-effect continuation snapshot");
}

static unsigned callm_args(CallmFixture *fx, IRRef call, IRRef *values,
                          IRRef *nodes)
{
  IRRef reversed[LJ_ARM64_ABI_MAX_ARGS], revnodes[LJ_ARM64_ABI_MAX_ARGS];
  IRRef ref = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  for (;;) {
    IRIns ins;
    assert(n < LJ_ARM64_ABI_MAX_ARGS &&
           ref >= trace_nk_acq(fx->owner) && ref < call);
    ins = ir_load_acq(&fx->ir[ref]);
    if (ins.o != IR_CARG) {
      reversed[n] = ref;
      revnodes[n++] = 0;
      break;
    }
    assert(ins.t.irt == IRT_NIL && ins.op1 < ref && ins.op2 < ref);
    reversed[n] = ins.op2;
    revnodes[n++] = ref;
    ref = ins.op1;
  }
  for (i = 0; i < n; i++) {
    values[i] = reversed[n-i-1u];
    nodes[i] = revnodes[n-i-1u];
  }
  return n;
}

static IRRef find_numeric_chain_root(CallmFixture *fx, IRRef value,
                                     IRRef numeric_result,
                                     IRRef *convrefp)
{
  IRRef ref = value, last = 0;
  while (ref != numeric_result) {
    IRIns ins;
    if (ref <= REF_FIRST || ref >= trace_nins_acq(fx->owner)) return 0;
    ins = ir_load_acq(&fx->ir[ref]);
    if (ins.o != IR_CONV || ins.op1 >= ref) return 0;
    last = ref;
    ref = ins.op1;
  }
  if (convrefp != NULL) *convrefp = last;
  return ref;
}

static void assert_callm_pairs(CallmFixture *fx, unsigned rawargs)
{
  unsigned producers = 0, consumers = 0;
  for (unsigned i = 0; i < fx->ncalls; i++) {
    BCIns src = source_call(fx, i, NULL);
    if (bc_op(src) == BC_CALL) {
      assert(bc_b(src) == 0);
      producers++;
      continue;
    }
    {
      IRRef values[LJ_ARM64_ABI_MAX_ARGS], nodes[LJ_ARM64_ABI_MAX_ARGS];
      unsigned n = callm_args(fx, fx->calls[i], values, nodes), j;
      unsigned producer_for_consumer = fx->ncalls;
      unsigned actual = n;
      IRRef carrier_ref;
      assert(bc_op(src) == BC_CALLM && n == rawargs);
      for (unsigned a = 0; a < n; a++) assert(values[a] != REF_NIL);
      actual = n;
      assert(bc_c(src) == actual-1u);
      for (j = 0; j < i; j++) {
        BCIns prev = source_call(fx, j, NULL);
        if (bc_op(prev) != BC_CALL || source_returnpc(fx, j) !=
            source_returnpc(fx, i)-1) continue;
        /* Preheader and loop share source bytecode. The final matching native
        ** producer is the one whose result is live at this consumer. */
        assert(bc_b(prev) == 0 && fx->convs[j] < fx->calls[i]);
        producer_for_consumer = j;
      }
      assert(producer_for_consumer < i &&
             "CALLM source must have an adjacent native producer");
      carrier_ref = values[n-1u];
      assert(find_numeric_chain_root(fx, carrier_ref,
                 fx->family == 2 ? fx->calls[producer_for_consumer] :
                                   fx->convs[producer_for_consumer], NULL) != 0);
      if (fx->convs[producer_for_consumer] !=
          fx->calls[producer_for_consumer]) {
        IRIns result = ir_load_acq(&fx->ir[fx->calls[producer_for_consumer]]);
        IRIns number = ir_load_acq(&fx->ir[fx->convs[producer_for_consumer]]);
        assert(number.o == IR_CONV && number.op1 == fx->calls[producer_for_consumer] &&
               number.t.irt == IRT_NUM);
        assert(number.op2 == ((IRT_NUM << IRCONV_DSH) | irt_type(result.t)));
      }
      consumers++;
    }
  }
  assert(producers == 2 && consumers == 2);
}

static unsigned selected_pair(CallmFixture *fx, unsigned *producerp,
                              IRRef *values, IRRef *nodes)
{
  unsigned consumer = fx->ncalls, producer = fx->ncalls, n;
  for (unsigned i = 0; i < fx->ncalls; i++)
    if (bc_op(source_call(fx, i, NULL)) == BC_CALLM) consumer = i;
  assert(consumer < fx->ncalls);
  for (unsigned i = 0; i < consumer; i++)
    if (bc_op(source_call(fx, i, NULL)) == BC_CALL &&
        source_returnpc(fx, i) == source_returnpc(fx, consumer)-1)
      producer = i;
  assert(producer < consumer);
  n = callm_args(fx, fx->calls[consumer], values, nodes);
  assert(n >= 1);
  *producerp = producer;
  return consumer;
}

static void replace_callm_arg(CallmFixture *fx, IRRef call, IRRef node,
                              IRRef replacement)
{
  if (node == 0) {
    fx->ir[call].op1 = replacement;
  } else {
    assert(fx->ir[node].o == IR_CARG);
    fx->ir[node].op2 = replacement;
  }
}

static IRRef numeric_conversion_from(CallmFixture *fx, IRRef carrier,
                                     IRRef source)
{
  IRRef r = carrier;
  while (r != source) {
    IRIns ins;
    if (r <= REF_FIRST || r >= trace_nins_acq(fx->owner)) return 0;
    ins = ir_load_acq(&fx->ir[r]);
    if (ins.o != IR_CONV || ins.op1 >= r) return 0;
    if (ins.op1 == source) {
      if (fx->family == 2) {
        if (ins.t.irt == IRT_INT || ins.t.irt == IRT_U32 ||
            ins.t.irt == IRT_I64 || ins.t.irt == IRT_U64) return r;
      } else if ((ins.t.irt == IRT_I64 || ins.t.irt == IRT_U64) &&
                 ins.op2 == (((uint32_t)ins.t.irt << IRCONV_DSH) |
                             IRT_NUM | IRCONV_ANY)) return r;
    }
    r = ins.op1;
  }
  return 0;
}

static void mutate_callm(CallmFixture *fx, enum Mutation m)
{
  unsigned producer;
  IRRef values[LJ_ARM64_ABI_MAX_ARGS], nodes[LJ_ARM64_ABI_MAX_ARGS];
  unsigned consumer = selected_pair(fx, &producer, values, nodes);
  unsigned n = callm_args(fx, fx->calls[consumer], values, nodes);
  IRRef tail = values[n-1u];
  IRRef chain_source = fx->family == 2 ? fx->calls[producer] : fx->convs[producer];
  IRRef integer_conv = numeric_conversion_from(fx, tail, chain_source);
  switch (m) {
  case M_CALLM_RAW_TAIL:
    replace_callm_arg(fx, fx->calls[consumer], nodes[n-1u],
                      fx->calls[producer]);
    break;
  case M_CALLM_WRONG_PRODUCER: {
    unsigned other_consumer = fx->ncalls;
    IRRef ov[LJ_ARM64_ABI_MAX_ARGS], on[LJ_ARM64_ABI_MAX_ARGS];
    for (unsigned i = 0; i < fx->ncalls; i++)
      if (i != consumer && bc_op(source_call(fx, i, NULL)) == BC_CALLM)
        other_consumer = i;
    assert(other_consumer < fx->ncalls);
    (void)callm_args(fx, fx->calls[other_consumer], ov, on);
    replace_callm_arg(fx, fx->calls[consumer], nodes[n-1u], ov[n-1u]);
    break;
  }
  case M_CALLM_BAD_CONV_MODE:
    assert(integer_conv != 0);
    fx->ir[integer_conv].op2 ^= IRCONV_ANY;
    break;
  case M_CALLM_BAD_CONV_FLAGS:
    assert(integer_conv != 0);
    fx->ir[integer_conv].t.irt |= IRT_GUARD;
    break;
  case M_CALLM_BAD_CONV_SOURCE: {
    unsigned other = fx->ncalls;
    assert(integer_conv != 0);
    for (unsigned i = 0; i < fx->ncalls; i++)
      if (i != producer && bc_op(source_call(fx, i, NULL)) == BC_CALL) {
        other = i;
        break;
      }
    assert(other < fx->ncalls);
    fx->ir[integer_conv].op1 = fx->family == 2 ? fx->calls[other] : fx->convs[other];
    break;
  }
  case M_CALLM_BAD_RESULT_MODE:
    assert(fx->family != 0 && fx->convs[producer] != fx->calls[producer]);
    fx->ir[fx->convs[producer]].op2 =
        ((IRT_NUM << IRCONV_DSH) | (fx->family == 1 ? IRT_U32 : IRT_FLOAT));
    break;
  case M_CALLM_BAD_RESULT_SOURCE: {
    unsigned other = fx->ncalls;
    assert(fx->family != 0 && fx->convs[producer] != fx->calls[producer]);
    for (unsigned i = 0; i < fx->ncalls; i++)
      if (i != producer && bc_op(source_call(fx, i, NULL)) == BC_CALL) {
        other = i;
        break;
      }
    assert(other < fx->ncalls);
    fx->ir[fx->convs[producer]].op1 = fx->calls[other];
    break;
  }
  case M_CALLM_BAD_PRODUCER_TYPE:
    fx->ir[fx->calls[producer]].t.irt = IRT_INT;
    break;
  case M_CALLM_ABI_SIGN: {
    static const unsigned widths[CALLM_KINDS] = {8,8,16,16,32,32,64,64};
    char other[48];
    CTypeID id;
    IRRef ctype = 0;
    IRRef fn = ir_load_acq(&fx->ir[fx->calls[consumer]]).op2;
    IRIns ptr = ir_load_acq(&fx->ir[fn]);
    IRRef cdata = ptr.o == IR_FLOAD && ptr.op2 == IRFL_CDATA_PTR ?
                  ptr.op1 : 0;
    const char *prefix = (fx->kind & 1u) ? "i" : "u";
    (void)snprintf(other, sizeof(other), "__callm_num_%s%u", prefix,
                   widths[fx->kind]);
    id = global_ctype(fx->L, other);
    assert(cdata != 0);
    for (IRRef r = REF_FIRST; r < fx->calls[consumer]; r++) {
      IRIns fld = ir_load_acq(&fx->ir[r]);
      if (fld.o != IR_FLOAD || fld.op2 != IRFL_CDATA_CTYPEID ||
          fld.op1 != cdata) continue;
      for (IRRef q = r+1u; q < fx->calls[consumer]; q++) {
        IRIns eq = ir_load_acq(&fx->ir[q]);
        IRRef k;
        if (eq.o != IR_EQ || !irt_isguard(eq.t) ||
            (eq.op1 != r && eq.op2 != r)) continue;
        k = eq.op1 == r ? eq.op2 : eq.op1;
        if (k < trace_nk_acq(fx->owner) ||
            ir_load_acq(&fx->ir[k]).o != IR_KINT) continue;
        ctype = k;
        break;
      }
      if (ctype) break;
    }
    assert(ctype != 0);
    fx->ir[ctype].i = (int32_t)id;
    break;
  }
  case M_CALLM_SOURCE_WRONG: {
    BCIns src = source_call(fx, consumer, NULL);
    BCReg slot = (BCReg)(1u + LJ_FR2 + bc_a(src) + 1u + LJ_FR2 + bc_c(src) + 1u);
    unsigned other = fx->ncalls;
    for (unsigned i = 0; i < fx->ncalls; i++)
      if (i != producer && bc_op(source_call(fx, i, NULL)) == BC_CALL)
        other = i;
    assert(other < fx->ncalls && fx->convs[other] < fx->xsaves[consumer]);
    change_slot(fx, fx->xsaves[consumer], slot, fx->convs[other], 0, 0);
    break;
  }
  case M_CALLM_U32_RAW_SNAPSHOT: {
    BCIns src;
    BCReg slot;
    assert(fx->family == 2);
    src = source_call(fx, consumer, NULL);
    slot = (BCReg)(1u + LJ_FR2 + bc_a(src) + 1u + LJ_FR2 + bc_c(src) + 1u);
    /* The Lua-visible producer result is its NUM conversion. A raw U32 ABI
    ** result must never appear in the consumer's restart frame. */
    change_slot(fx, fx->xsaves[consumer], slot, fx->calls[producer], 0, 0);
    break;
  }
  case M_CALLM_U32_CARRIER_USE:
    assert(fx->family == 2 && integer_conv != 0);
    add_side_use(fx, integer_conv);
    break;
  case M_CALLM_STACK_U32_CARRIER:
    assert(fx->family == 2 && fx->stack && n == 12 && nodes[10] != 0);
    /* Keep the C integer type valid but route the U32 producer carrier into
    ** the fixed U32 sentinel argument instead of the final multires slot. */
    replace_callm_arg(fx, fx->calls[consumer], nodes[10],
                      fx->calls[producer]);
    break;
  case M_CALLM_STACK_U32_CONSTANT:
    assert(fx->family == 2 && fx->stack && n == 12 && nodes[10] != 0 &&
           irt_type(ir_load_acq(&fx->ir[values[0]]).t) == IRT_INT &&
           irt_type(ir_load_acq(&fx->ir[values[10]]).t) == IRT_INT);
    /* Preserve the native KINT type while substituting the wrong sentinel. */
    replace_callm_arg(fx, fx->calls[consumer], nodes[10], values[0]);
    break;
  case M_CALLM_STACK_U32_SNAPSHOT: {
    BCIns src;
    BCReg slot;
    assert(fx->family == 2 && fx->stack && n == 12);
    src = source_call(fx, consumer, NULL);
    slot = (BCReg)(1u + LJ_FR2 + bc_a(src) + 1u + LJ_FR2 + 11u);
    /* Corrupt only the saved Lua sentinel; keep the native KINT carrier. */
    change_slot(fx, fx->xsaves[consumer], slot, values[0], 0, 0);
    break;
  }
  case M_CALLM_STACK_INT_CARRIER:
    assert(fx->family == 2 && fx->stack && n == 12 && integer_conv != 0 &&
           nodes[1] != 0 && fx->ir[nodes[1]].o == IR_CARG);
    /* Reuse the same-width INT carrier in the first INT argument. Its native
    ** type is plausible, but the caller slot still contains the sentinel. */
    fx->ir[nodes[1]].op1 = integer_conv;
    break;
  case M_CALLM_CARRIER_SNAPSHOT:
    assert(fx->stack && (fx->kind == 1 || fx->kind == 3) &&
           integer_conv != 0);
    carrier_in_unrelated_loop_snapshot(fx, tail, consumer);
    break;
  case M_CALLM_PRODUCER_WRONG_RESULT:
  case M_CALLM_PRODUCER_NORESTORE: {
    BCReg slot;
    unsigned other = fx->ncalls;
    BCIns src;
    assert(fx->family != 2 && fx->convs[producer] != 0);
    src = source_call(fx, producer, &slot);
    assert(bc_op(src) == BC_CALL);
    if (m == M_CALLM_PRODUCER_WRONG_RESULT) {
      for (unsigned i = 0; i < fx->ncalls; i++)
        if (i != producer && bc_op(source_call(fx, i, NULL)) == BC_CALL) {
          other = i;
          break;
        }
      assert(other < fx->ncalls && fx->convs[other] != 0);
      change_slot(fx, fx->leaves[producer], slot, fx->convs[other], 0, 0);
    } else {
      change_slot(fx, fx->leaves[producer], slot, fx->convs[producer],
                  SNAP_NORESTORE, 0);
    }
    break;
  }
  case M_CALLM_CONSUMER_NORESTORE:
  case M_CALLM_CONSUMER_MISSING:
  case M_CALLM_CONSUMER_BAD_PC:
  case M_CALLM_CONSUMER_BAD_COUNT: {
    BCReg slot;
    (void)source_call(fx, consumer, &slot);
    if (m == M_CALLM_CONSUMER_BAD_PC || m == M_CALLM_CONSUMER_BAD_COUNT) {
      MSize s;
      for (s = 0; s < trace_nsnap_acq(fx->owner); s++)
        if (snap_ref_acq(&fx->snap[s]) == fx->leaves[consumer]) break;
      assert(s < trace_nsnap_acq(fx->owner));
      if (m == M_CALLM_CONSUMER_BAD_COUNT) fx->snap[s].count = 1;
      else {
        MSize ofs = snap_mapofs_acq(&fx->snap[s]);
        MSize nent = snap_nent_acq(&fx->snap[s]);
        uint64_t footer;
        memcpy(&footer, &fx->snapmap[ofs+nent], sizeof(footer));
        footer = (footer & UINT64_C(0xff)) |
          ((uint64_t)(uintptr_t)(source_returnpc(fx, consumer)-1) << 8);
        memcpy(&fx->snapmap[ofs+nent], &footer, sizeof(footer));
      }
    } else {
      change_slot(fx, fx->leaves[consumer], slot, fx->convs[consumer],
                  m == M_CALLM_CONSUMER_NORESTORE ? SNAP_NORESTORE : 0,
                  m == M_CALLM_CONSUMER_MISSING);
    }
    break;
  }
  case M_CALLM_BAD_BANK:
    assert(integer_conv != 0);
    fx->ir[tail].r = RID_D0;
    fx->ir[tail].s = SPS_NONE;
    break;
  case M_CALLM_BAD_SPILL_ALIGN:
    assert(integer_conv != 0 && fx->kind >= 6);
    fx->ir[tail].r = RID_NONE;
    fx->ir[tail].s = (uint8_t)(SPS_FIRST + 1u);
    break;
  case M_CALLM_BAD_SPILL_EXTENT: {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx->owner) / sizeof(int32_t);
    assert(integer_conv != 0 && cap < UINT8_MAX);
    fx->ir[tail].r = RID_NONE;
    fx->ir[tail].s = (uint8_t)cap;
    break;
  }
  default:
    assert(!"unknown numeric CALLM mutation");
  }
}

static void both(CallmFixture *fx, jit_State *J, enum Mutation m)
{
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize idx = (MSize)((start-base)/sizeof(BCIns));
  BCIns saved = loadbc(&bc[idx]);
  JSave js;
  active_mutation = (int)m;
  assert(idx < fx->pt->sizebc && bc_op(saved) == BC_JFORL);
  reset_fixture(fx);
  mutate_callm(fx, m);
  bc_publish((const uint32_t *)&bc[idx], trace_startins_acq(fx->owner));
  js = enter_semantic(J, fx);
  expect_semantic(J, fx, 0);
  leave_semantic(J, &js);
  bc_publish((const uint32_t *)&bc[idx], saved);
  reset_fixture(fx);
  mutate_callm(fx, m);
  {
    LJArm64PostRAView v = postra_view(fx);
    expect_postra(&v, 0);
  }
}

static void postra_negative(CallmFixture *fx, enum Mutation m)
{
  IRRef values[LJ_ARM64_ABI_MAX_ARGS], nodes[LJ_ARM64_ABI_MAX_ARGS];
  unsigned producer, consumer, n;
  IRRef tail;
  LJArm64PostRAView view;
  reset_fixture(fx);
  /* Re-resolve refs after restoring the pristine cloned view. */
  consumer = selected_pair(fx, &producer, values, nodes);
  n = callm_args(fx, fx->calls[consumer], values, nodes);
  tail = values[n-1u];
  if (m == M_CALLM_BAD_BANK) {
    fx->ir[tail].r = RID_D0;
    fx->ir[tail].s = SPS_NONE;
  } else if (m == M_CALLM_BAD_SPILL_ALIGN) {
    fx->ir[tail].r = RID_NONE;
    fx->ir[tail].s = (uint8_t)(SPS_FIRST + 1u);
  } else if (m == M_CALLM_BAD_SPILL_EXTENT) {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx->owner) / sizeof(int32_t);
    assert(cap < UINT8_MAX);
    fx->ir[tail].r = RID_NONE;
    fx->ir[tail].s = (uint8_t)cap;
  } else {
    assert(!"unknown post-RA negative");
  }
  active_mutation = (int)m;
  view = postra_view(fx);
  expect_postra(&view, 0);
}

static void positive(CallmFixture *fx, jit_State *J) {
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize idx = (MSize)((start - base) / sizeof(BCIns));
  BCIns saved = loadbc(&bc[idx]);
  JSave js;
  assert(idx < fx->pt->sizebc && bc_op(saved) == BC_JFORL);
  bc_publish((const uint32_t *)&bc[idx], trace_startins_acq(fx->owner));
  reset_fixture(fx);
  js = enter_semantic(J, fx);
  expect_semantic(J, fx, 1);
  leave_semantic(J, &js);
  bc_publish((const uint32_t *)&bc[idx], saved);
  reset_fixture(fx);
  {
    LJArm64PostRAView v = postra_view(fx);
    expect_postra(&v, 1);
  }
}
static void both_source_mutation(CallmFixture *fx, jit_State *J, int mode)
{
  unsigned producer = fx->ncalls, consumer = fx->ncalls;
  const BCIns *bc = proto_bc(fx->pt);
  size_t pi, ci;
  BCIns saved_p, saved_c;
  JSave js;
  active_mutation = -2-mode;
  for (unsigned i = 0; i < fx->ncalls; i++) {
    BCIns ins = source_call(fx, i, NULL);
    if (bc_op(ins) == BC_CALL && producer == fx->ncalls) producer = i;
    if (bc_op(ins) == BC_CALLM) { consumer = i; break; }
  }
  assert(producer < consumer && consumer < fx->ncalls);
  {
    const BCIns *pr = source_returnpc(fx, producer);
    const BCIns *cr = source_returnpc(fx, consumer);
    assert(pr + 1 == cr);
    pi = (size_t)(pr-bc)-1u;
    ci = (size_t)(cr-bc)-1u;
  }
  saved_p = loadbc(&bc[pi]);
  saved_c = loadbc(&bc[ci]);
  reset_fixture(fx);
  if (mode == 0)
    bc_publish((const uint32_t *)&bc[pi], (saved_p & UINT32_C(0x00ffffff)) |
               (UINT32_C(1) << 24));
  else if (mode == 1)
    bc_publish((const uint32_t *)&bc[ci], (saved_c & UINT32_C(0xff00ffff)) |
               ((uint32_t)(bc_c(saved_c)+1u) << 16));
  else if (mode == 2)
    bc_publish((const uint32_t *)&bc[ci],
               (saved_c & UINT32_C(0xffffff00)) | BC_CALL);
  else {
    uint32_t a = (uint32_t)bc_a(saved_p) + 1u;
    assert(mode == 3 && a <= UINT8_MAX);
    bc_publish((const uint32_t *)&bc[pi],
               (saved_p & UINT32_C(0xffff00ff)) | (a << 8));
  }
  {
    uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
    uintptr_t base = (uintptr_t)(const void *)bc;
    MSize idx = (MSize)((start-base)/sizeof(BCIns));
    BCIns saved_start = loadbc(&bc[idx]);
    assert(idx < fx->pt->sizebc && bc_op(saved_start) == BC_JFORL);
    bc_publish((const uint32_t *)&bc[idx], trace_startins_acq(fx->owner));
    js = enter_semantic(J, fx);
    expect_semantic(J, fx, 0);
    leave_semantic(J, &js);
    bc_publish((const uint32_t *)&bc[idx], saved_start);
  }
  {
    reset_fixture(fx);
    LJArm64PostRAView v = postra_view(fx);
    expect_postra(&v, 0);
  }
  bc_publish((const uint32_t *)&bc[pi], saved_p);
  bc_publish((const uint32_t *)&bc[ci], saved_c);
  reset_fixture(fx);
}

static void run_callm(lua_State *L, unsigned family, unsigned kind, int stack,
                      const char *runner)
{
  lua_getglobal(L, "__callm_num_configure");
  lua_pushinteger(L, (lua_Integer)family);
  lua_pushinteger(L, (lua_Integer)kind);
  lua_pushboolean(L, stack);
  assert(lua_pcall(L, 3, 0, 0) == LUA_OK);
  lua_getglobal(L, "__callm_num_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, "__callm_num_table");
  {
    char name[64];
    (void)snprintf(name, sizeof(name), "__callm_num_produce_%s",
                   family_names[family]);
    lua_getglobal(L, name);
    (void)snprintf(name, sizeof(name), "__callm_num_%s%s",
                   stack ? "stack_" : "", kind_names[kind]);
    lua_getglobal(L, name);
  }
  lua_pushinteger(L, CALLM_RUNS);
  if (lua_pcall(L, 5, 1, 0) != 0) {
    fprintf(stderr, "numeric CALLM runner %s failed: %s\n", runner,
            lua_tostring(L, -1));
    abort();
  }
  assert(lua_isnumber(L, -1) && lua_tonumber(L, -1) == CALLM_RUNS * 1.25);
  lua_pop(L, 1);
}

static void warm_callm_trace(lua_State *L, jit_State *J, unsigned family,
                             unsigned kind, int stack, const char *runner)
{
  GCproto *pt = global_proto(L, runner);
  unsigned attempt;
  for (attempt = 0; attempt < 8u; attempt++) {
    /* A recorder abort caused by a concurrent hook can consume the first
    ** cold attempt without publishing a trace. Re-arm the threshold only;
    ** keep every already-published runner trace in the trace table. */
    ljt_lua_dostring(L,
      "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=256')");
    run_callm(L, family, kind, stack, runner);
    if (find_trace(J, pt, 4)) return;
  }
  fprintf(stderr, "numeric CALLM trace did not publish after 8 attempts: %s\n",
          runner);
  abort();
}

static void assert_consumer_abi(lua_State *L, const char *consumer,
                                unsigned kind, int stack)
{
  LJArm64ABISignature abi;
  CTypeID id;
  static const uint8_t widths[CALLM_KINDS] = {1,1,2,2,4,4,8,8};
  static const uint8_t uns[CALLM_KINDS] = {0,1,0,1,0,1,0,1};
  unsigned last = stack ? 11u : 0u;
  lua_getglobal(L, consumer);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)), id, &abi));
  assert(!abi.is_vararg && abi.result_class == LJ_ARM64_ABI_FPR &&
         abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT && abi.result_size == 8);
  assert(abi.nargs == (stack ? 12u : 1u));
  assert(abi.arg[last].kind == LJ_ARM64_ABI_KIND_INTEGER &&
         abi.arg[last].size == widths[kind] &&
         abi.arg[last].is_unsigned == uns[kind]);
}

static void exercise_negatives(CallmFixture *fx, jit_State *J,
                               unsigned index)
{
  if (index == 0) {
    both(fx, J, M_CALLM_RAW_TAIL);
    both(fx, J, M_CALLM_WRONG_PRODUCER);
    both(fx, J, M_CALLM_BAD_CONV_MODE);
    both(fx, J, M_CALLM_BAD_CONV_FLAGS);
    both(fx, J, M_CALLM_BAD_CONV_SOURCE);
    both(fx, J, M_CALLM_BAD_PRODUCER_TYPE);
    both(fx, J, M_CALLM_ABI_SIGN);
    both(fx, J, M_CALLM_SOURCE_WRONG);
    both(fx, J, M_CALLM_CONSUMER_NORESTORE);
    both(fx, J, M_CALLM_CONSUMER_MISSING);
    both(fx, J, M_CALLM_CONSUMER_BAD_PC);
    both(fx, J, M_CALLM_CONSUMER_BAD_COUNT);
    both_source_mutation(fx, J, 0);
    both_source_mutation(fx, J, 1);
    both_source_mutation(fx, J, 2);
    both_source_mutation(fx, J, 3);
    postra_negative(fx, M_CALLM_BAD_BANK);
    both(fx, J, M_CALLM_PRODUCER_WRONG_RESULT);
    both(fx, J, M_CALLM_PRODUCER_NORESTORE);
  }
  if (index == 3 || index == 7 || index == 19 || index == 23 ||
      index == 35 || index == 39)
    both(fx, J, M_CALLM_CARRIER_SNAPSHOT);
  if (index == 9)
    both(fx, J, M_CALLM_RAW_TAIL);
  if (index == 14)
    postra_negative(fx, M_CALLM_BAD_SPILL_ALIGN);
  if (index == 16) {
    both_source_mutation(fx, J, 0);
    both(fx, J, M_CALLM_BAD_CONV_SOURCE);
    both(fx, J, M_CALLM_BAD_RESULT_MODE);
    both(fx, J, M_CALLM_BAD_RESULT_SOURCE);
    both(fx, J, M_CALLM_PRODUCER_WRONG_RESULT);
    both(fx, J, M_CALLM_PRODUCER_NORESTORE);
  }
  if (index == 25)
    both_source_mutation(fx, J, 1);
  if (index == 32) {
    both(fx, J, M_CALLM_U32_RAW_SNAPSHOT);
    both(fx, J, M_CALLM_U32_CARRIER_USE);
    both_source_mutation(fx, J, 0);
    both(fx, J, M_CALLM_BAD_CONV_SOURCE);
    both(fx, J, M_CALLM_BAD_RESULT_MODE);
    both(fx, J, M_CALLM_BAD_RESULT_SOURCE);
  }
  if (index == 33) {
    both(fx, J, M_CALLM_STACK_U32_CARRIER);
    both(fx, J, M_CALLM_STACK_U32_CONSTANT);
    both(fx, J, M_CALLM_STACK_U32_SNAPSHOT);
    both(fx, J, M_CALLM_STACK_INT_CARRIER);
  }
  if (index == 47) {
    both(fx, J, M_CALLM_BAD_CONV_MODE);
    postra_negative(fx, M_CALLM_BAD_SPILL_EXTENT);
  }
}

int main(void)
{
  enum { TOTAL = CALLM_FAMILIES * CALLM_KINDS * 2 };
  CallmFixture fx[TOTAL] = {{0}};
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  unsigned index = 0;
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callm_numeric_configure(int32_t,int32_t,int32_t);\n"
    "double lj_callm_numeric_producer_double(double); float lj_callm_numeric_producer_float(float); uint32_t lj_callm_numeric_producer_u32(uint32_t);\n"
    "double lj_callm_numeric_i8(int8_t); double lj_callm_numeric_u8(uint8_t); double lj_callm_numeric_i16(int16_t); double lj_callm_numeric_u16(uint16_t);\n"
    "double lj_callm_numeric_i32(int32_t); double lj_callm_numeric_u32(uint32_t); double lj_callm_numeric_i64(int64_t); double lj_callm_numeric_u64(uint64_t);\n"
    "double lj_callm_numeric_stack_i8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int8_t);\n"
    "double lj_callm_numeric_stack_u8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint8_t);\n"
    "double lj_callm_numeric_stack_i16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int16_t);\n"
    "double lj_callm_numeric_stack_u16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint16_t);\n"
    "double lj_callm_numeric_stack_i32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int32_t);\n"
    "double lj_callm_numeric_stack_u32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint32_t);\n"
    "double lj_callm_numeric_stack_i64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int64_t);\n"
    "double lj_callm_numeric_stack_u64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint64_t);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLM_NUMERIC_SO')))\n"
    "local fam={'double','float','u32'}; local kinds={'i8','u8','i16','u16','i32','u32','i64','u64'}\n"
    "for _,f in ipairs(fam) do _G['__callm_num_produce_'..f]=lib['lj_callm_numeric_producer_'..f] end\n"
    "for _,k in ipairs(kinds) do _G['__callm_num_'..k]=lib['lj_callm_numeric_'..k]; _G['__callm_num_stack_'..k]=lib['lj_callm_numeric_stack_'..k] end\n"
    "local unary='return function(t,p,c,n) local s=.5; for i=1,n do s=s+c(p(t.value)) end; return s-.5 end'\n"
    "local stacked='return function(t,p,c,n) local s=.5; for i=1,n do s=s+c(101,102,103,104,105,106,107,108,31,-505,1985229328,p(t.value)) end; return s-.5 end'\n"
    "for _,f in ipairs(fam) do for _,k in ipairs(kinds) do\n"
    "_G['__callm_num_run_'..f..'_'..k]=assert(loadstring(unary))()\n"
    "_G['__callm_num_run_stack_'..f..'_'..k]=assert(loadstring(stacked))()\n"
    "end end\n"
    "function _G.__callm_num_configure(f,k,s) lib.lj_callm_numeric_configure(f,k,s and 1 or 0) end\n"
    "function _G.__callm_num_driver(run,t,p,c,n) local result=run(t,p,c,n); return result end\n"
    "_G.__callm_num_table={value=-3.75}\n"
    "local threading=require('threading'); _G.__callm_num_ready=threading.channel(1); _G.__callm_num_release=threading.channel(1); _G.__callm_num_peer=assert(threading.spawn(function(ready,release) assert(ready:send(true,5)); assert(select(2,release:recv(60))); return true end,__callm_num_ready,__callm_num_release)); assert(select(2,__callm_num_ready:recv(5)))\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=256')\n");

  for (unsigned family = 0; family < CALLM_FAMILIES; family++)
    for (unsigned kind = 0; kind < CALLM_KINDS; kind++)
      for (unsigned stack = 0; stack < 2; stack++) {
        char runner[80], consumer[80];
        (void)snprintf(runner, sizeof(runner), stack ?
          "__callm_num_run_stack_%s_%s" : "__callm_num_run_%s_%s",
          family_names[family], kind_names[kind]);
        (void)snprintf(consumer, sizeof(consumer), "__callm_num_%s%s",
          stack ? "stack_" : "", kind_names[kind]);
        fx[index].family = family;
        fx[index].kind = kind;
        fx[index].stack = (int)stack;
        assert_consumer_abi(L, consumer, kind, stack != 0);
        warm_callm_trace(L, J, family, kind, stack != 0, runner);
        init_fixture(L, J, &fx[index], runner, family, kind, stack != 0, 4);
        assert_callm_pairs(&fx[index], stack ? 12u : 1u);
        for (unsigned c = 0; c < fx[index].ncalls; c++)
          assert_result_continuation(&fx[index], c);
        positive(&fx[index], J);
        exercise_negatives(&fx[index], J, index);
        index++;
      }
  ljt_lua_dostring(L,
    "assert(__callm_num_release:send(true,5)); local ok,v=__callm_num_peer:join(5); assert(ok and v)");

  for (unsigned i = 0; i < TOTAL; i++) {
    free(fx[i].ir);
    free(fx[i].snap);
    free(fx[i].snapmap);
  }
  lua_close(L);
  printf("numeric CALLM semantic=%u/%u post-RA=%u/%u\n", semantic_accepts,
         semantic_rejects, postra_accepts, postra_rejects);
  puts("t-arm64-jit-callm-numeric-admission OK");
  return 0;
}
#else
int main(void) {
  puts("t-arm64-jit-callm-numeric-admission SKIP");
  return 0;
}
#endif
