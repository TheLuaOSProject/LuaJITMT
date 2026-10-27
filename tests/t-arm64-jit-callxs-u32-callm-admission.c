/* Independent semantic and post-RA admission tests for U32 CALLM chains. */

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
#error "t-arm64-jit-callxs-u32-callm-admission requires ARM64 CALLXS admission"
#endif

typedef struct U32Fixture {
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, ktrace;
  IRRef calls[8], convs[8], xsaves[8], leaves[8];
  unsigned ncalls;
  int discarded;
  GCtrace view_owner, semantic;
} U32Fixture;

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

static void locate_calls(U32Fixture *fx) {
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
    for (r = ref + 1u; r < trace_nins_acq(fx->owner); r++) {
      IRIns next = ir_load_acq(&fx->ir[r]);
      if (next.o == IR_CONV && next.op1 == ref &&
          next.op2 == ((IRT_NUM << IRCONV_DSH) | IRT_U32) &&
          (next.t.irt & ~IRT_ISPHI) == IRT_NUM) {
        fx->convs[fx->ncalls] = r;
        break;
      }
      if (next.o == IR_CALLS && next.op2 == IRCALL_lj_ffi_native_trace_leave)
        break;
    }
    if (!fx->discarded)
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

static void init_fixture(lua_State *L, jit_State *J, U32Fixture *fx,
                         const char *runner, unsigned calls, int discarded) {
  fx->pt = global_proto(L, runner);
  fx->owner = find_trace(J, fx->pt, calls);
  if (!fx->owner)
    fprintf(stderr, "U32 CALLXS trace missing for %s\n", runner);
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
  fx->discarded = discarded;
  locate_calls(fx);
  assert(fx->ncalls == calls);
}

static void reset_fixture(U32Fixture *fx) {
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

static JSave enter_semantic(jit_State *J, U32Fixture *fx) {
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

static LJArm64PostRAView postra_view(U32Fixture *fx) {
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

static void expect_semantic(jit_State *J, U32Fixture *fx, int accepted) {
  LJArm64IRReject reject;
  int got = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (got != accepted)
    fprintf(
        stderr,
        "U32 semantic mutation=%d expected %d got %d reason=%d ref=%u op=%u detail=%u\n",
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
    fprintf(stderr, "U32 post-RA mutation=%d expected %d got %d\n",
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
  M_CALLM_BAD_MARKER,
  M_CALLM_DUP_MARKER,
  M_CALLM_BAD_PRODUCER_TYPE,
  M_CALLM_PRODUCER_NORESTORE,
  M_CALLM_PRODUCER_MISSING,
  M_CALLM_PRODUCER_WRONG,
  M_CALLM_CONSUMER_NORESTORE,
  M_CALLM_CONSUMER_MISSING,
  M_CALLM_CONSUMER_WRONG,
  M_CALLM_PRODUCER_BAD_PC,
  M_CALLM_CONSUMER_BAD_PC,
  M_CALLM_PRODUCER_BAD_COUNT,
  M_CALLM_CONSUMER_BAD_COUNT,
  M_CALLM_SOURCE_WRONG,
  M_CALLM_FLOAT_MODE,
  M_CALLM_FLOAT_FLAGS,
  M_CALLM_FLOAT_SOURCE
};

static void change_slot(U32Fixture *fx, IRRef snapref, BCReg slot,
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

static BCIns source_call(U32Fixture *fx, unsigned ix, BCReg *slotp) {
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

static const BCIns *source_returnpc(U32Fixture *fx, unsigned ix) {
  IRIns marker = ir_load_acq(&fx->ir[fx->xsaves[ix]]);
  uint32_t offset = (uint32_t)marker.op1 | ((uint32_t)marker.op2 << 15);
  assert(marker.o == IR_XSAVE && offset > 0 && offset < fx->pt->sizebc);
  return &proto_bc(fx->pt)[offset];
}

static void assert_result_continuation(U32Fixture *fx, unsigned ix) {
  BCReg slot;
  BCIns ins = source_call(fx, ix, &slot);
  MSize s;
  assert(bc_b(ins) != 1 && fx->convs[ix] != 0);
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++) {
    if (snap_ref_acq(&fx->snap[s]) == fx->leaves[ix]) {
      MSize ofs = snap_mapofs_acq(&fx->snap[s]);
      MSize n = snap_nent_acq(&fx->snap[s]), i;
      unsigned found = 0;
      assert(snap_count_acq(&fx->snap[s]) == SNAPCOUNT_DONE);
      assert(snap_pc(&fx->snapmap[ofs + n]) == source_returnpc(fx, ix));
      for (i = 0; i < n; i++)
        if (snap_slot(fx->snapmap[ofs + i]) == slot) {
          assert(fx->snapmap[ofs + i] == SNAP(slot, 0, fx->convs[ix]));
          found++;
        }
      assert(found == 1);
      return;
    }
  }
  assert(!"numeric U32 CALLXS lacks exact leave result snapshot");
}

static unsigned callm_args(U32Fixture *fx, IRRef call, IRRef *values,
                          IRRef *nodes)
{
  IRRef reversed[LJ_ARM64_ABI_MAX_ARGS], revnodes[LJ_ARM64_ABI_MAX_ARGS];
  IRRef ref = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  for (;;) {
    IRIns ins;
    assert(n < LJ_ARM64_ABI_MAX_ARGS && ref >= REF_FIRST && ref < call);
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

static void assert_callm_pairs(U32Fixture *fx, unsigned marker,
                               unsigned rawargs, IRType tailtype)
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
      if (marker != UINT_MAX) {
        assert(marker < n && values[marker] == REF_NIL);
        actual--;
      }
      for (unsigned a = 0; a < n; a++)
        if (a != marker) assert(values[a] != REF_NIL);
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
      {
        IRIns carrier = ir_load_acq(&fx->ir[carrier_ref]);
        if (carrier_ref != fx->convs[producer_for_consumer]) {
          assert(carrier.o == IR_CONV &&
                 carrier.op1 == fx->convs[producer_for_consumer]);
          assert(irt_type(carrier.t) == tailtype);
        } else {
          assert(tailtype == IRT_NUM);
        }
        carrier = ir_load_acq(&fx->ir[fx->convs[producer_for_consumer]]);
        assert(carrier.o == IR_CONV &&
               carrier.op1 == fx->calls[producer_for_consumer]);
        assert(carrier.op2 == ((IRT_NUM << IRCONV_DSH) | IRT_U32));
        assert(irt_type(carrier.t) == IRT_NUM);
      }
      consumers++;
    }
  }
  assert(producers == 2 && consumers == 2);
}

static void mutate_callm(U32Fixture *fx, enum Mutation m)
{
  unsigned consumer = fx->ncalls, producer = fx->ncalls;
  IRRef values[LJ_ARM64_ABI_MAX_ARGS], nodes[LJ_ARM64_ABI_MAX_ARGS];
  unsigned n;
  for (unsigned i = 0; i < fx->ncalls; i++)
    if (bc_op(source_call(fx, i, NULL)) == BC_CALLM) consumer = i;
  assert(consumer < fx->ncalls);
  for (unsigned i = 0; i < consumer; i++)
    if (bc_op(source_call(fx, i, NULL)) == BC_CALL &&
        source_returnpc(fx, i) == source_returnpc(fx, consumer)-1)
      producer = i;
  assert(producer < consumer);
  n = callm_args(fx, fx->calls[consumer], values, nodes);
  assert(n >= 2 && nodes[n-1u] != 0);
  switch (m) {
  case M_CALLM_SOURCE_WRONG: {
    BCIns src = source_call(fx, consumer, NULL);
    BCReg slot = (BCReg)(1u+LJ_FR2+bc_a(src)+1u+LJ_FR2+bc_c(src)+1u);
    assert(producer >= 2 && fx->convs[0] < fx->xsaves[consumer]);
    change_slot(fx, fx->xsaves[consumer], slot, fx->convs[0], 0, 0);
    break;
  }
  case M_CALLM_FLOAT_MODE:
  case M_CALLM_FLOAT_FLAGS:
  case M_CALLM_FLOAT_SOURCE: {
    IRIns *carrier = &fx->ir[values[n-1u]];
    assert(carrier->o == IR_CONV && carrier->t.irt == IRT_FLOAT &&
           carrier->op1 == fx->convs[producer]);
    if (m == M_CALLM_FLOAT_MODE)
      carrier->op2 = ((IRT_FLOAT << IRCONV_DSH)|IRT_INT);
    else if (m == M_CALLM_FLOAT_FLAGS)
      carrier->t.irt |= IRT_GUARD;
    else
      carrier->op1 = fx->convs[0];
    break;
  }
  case M_CALLM_PRODUCER_NORESTORE:
  case M_CALLM_PRODUCER_MISSING:
  case M_CALLM_PRODUCER_WRONG:
  case M_CALLM_CONSUMER_NORESTORE:
  case M_CALLM_CONSUMER_MISSING:
  case M_CALLM_CONSUMER_WRONG:
  case M_CALLM_PRODUCER_BAD_PC:
  case M_CALLM_CONSUMER_BAD_PC:
  case M_CALLM_PRODUCER_BAD_COUNT:
  case M_CALLM_CONSUMER_BAD_COUNT: {
    unsigned target = producer;
    BCReg slot;
    if (m == M_CALLM_CONSUMER_NORESTORE || m == M_CALLM_CONSUMER_MISSING ||
        m == M_CALLM_CONSUMER_WRONG || m == M_CALLM_CONSUMER_BAD_PC ||
        m == M_CALLM_CONSUMER_BAD_COUNT)
      target = consumer;
    (void)source_call(fx, target, &slot);
    if (m == M_CALLM_PRODUCER_BAD_PC || m == M_CALLM_CONSUMER_BAD_PC) {
      MSize snapno;
      for (snapno = 0; snapno < trace_nsnap_acq(fx->owner); snapno++)
        if (snap_ref_acq(&fx->snap[snapno]) == fx->leaves[target]) break;
      assert(snapno < trace_nsnap_acq(fx->owner));
      {
        MSize ofs = snap_mapofs_acq(&fx->snap[snapno]);
        MSize nent = snap_nent_acq(&fx->snap[snapno]);
        uint64_t footer;
        const BCIns *wrong = source_returnpc(fx, target)-1;
        memcpy(&footer, &fx->snapmap[ofs+nent], sizeof(footer));
        footer = (footer & UINT64_C(0xff)) | ((uint64_t)(uintptr_t)wrong << 8);
        memcpy(&fx->snapmap[ofs+nent], &footer, sizeof(footer));
      }
      break;
    }
    if (m == M_CALLM_PRODUCER_BAD_COUNT || m == M_CALLM_CONSUMER_BAD_COUNT) {
      MSize snapno;
      for (snapno = 0; snapno < trace_nsnap_acq(fx->owner); snapno++)
        if (snap_ref_acq(&fx->snap[snapno]) == fx->leaves[target]) {
          fx->snap[snapno].count = 1;
          break;
        }
      assert(snapno < trace_nsnap_acq(fx->owner));
      break;
    }
    {
      IRRef replacement = fx->convs[target];
      if (m == M_CALLM_PRODUCER_WRONG || m == M_CALLM_CONSUMER_WRONG) {
        unsigned other = fx->ncalls;
        for (unsigned i = 0; i < target; i++)
          if (i != producer && bc_op(source_call(fx, i, NULL)) == BC_CALL)
            other = i;
        assert(other < target);
        replacement = fx->convs[other];
      }
      change_slot(fx, fx->leaves[target], slot, replacement,
                  (m == M_CALLM_PRODUCER_NORESTORE ||
                   m == M_CALLM_CONSUMER_NORESTORE) ? SNAP_NORESTORE : 0,
                  m == M_CALLM_PRODUCER_MISSING ||
                  m == M_CALLM_CONSUMER_MISSING);
    }
    break;
  }
  case M_CALLM_BAD_PRODUCER_TYPE:
    fx->ir[fx->calls[producer]].t.irt = IRT_INT;
    break;
  case M_CALLM_RAW_TAIL:
    fx->ir[nodes[n-1u]].op2 = fx->calls[producer];
    break;
  case M_CALLM_WRONG_PRODUCER: {
    unsigned other = fx->ncalls;
    for (unsigned i = 0; i < consumer; i++)
      if (i != producer && bc_op(source_call(fx, i, NULL)) == BC_CALL) {
        other = i;
        break;
      }
    assert(other < fx->ncalls && fx->convs[other] != 0);
    fx->ir[nodes[n-1u]].op2 = fx->convs[other];
    break;
  }
  case M_CALLM_BAD_CONV_MODE:
    fx->ir[fx->convs[producer]].op2 = IRCONV_NUM_INT;
    break;
  case M_CALLM_BAD_CONV_FLAGS:
    fx->ir[fx->convs[producer]].t.irt = IRT_NUM | IRT_GUARD;
    break;
  case M_CALLM_BAD_CONV_SOURCE: {
    unsigned other = fx->ncalls;
    for (unsigned i = 0; i < producer; i++)
      if (bc_op(source_call(fx, i, NULL)) == BC_CALL) { other = i; break; }
    assert(other < producer);
    fx->ir[fx->convs[producer]].op1 = fx->calls[other];
    break;
  }
  case M_CALLM_BAD_MARKER:
    assert(n == 5 && nodes[1] != 0);
    fx->ir[nodes[1]].op2 = values[2];
    break;
  case M_CALLM_DUP_MARKER:
    fx->ir[nodes[n-1u]].op2 = REF_NIL;
    break;
  default:
    assert(!"unknown CALLM mutation");
  }
}

static void both(U32Fixture *fx, jit_State *J, enum Mutation m)
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

static void positive(U32Fixture *fx, jit_State *J) {
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
static void both_source_mutation(U32Fixture *fx, jit_State *J, int mode)
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
    LJArm64PostRAView v = postra_view(fx);
    expect_postra(&v, 0);
  }
  bc_publish((const uint32_t *)&bc[pi], saved_p);
  bc_publish((const uint32_t *)&bc[ci], saved_c);
  reset_fixture(fx);
}

static void run_callm(lua_State *L, const char *runner)
{
  lua_getglobal(L, "__u32_callm_driver");
  lua_getglobal(L, runner);
  lua_pushinteger(L, 64);
  if (lua_pcall(L, 2, 1, 0) != 0) {
    fprintf(stderr, "U32 CALLM runner %s failed: %s\n", runner,
            lua_tostring(L, -1));
    abort();
  }
  lua_pop(L, 1);
}

static void assert_consumer_abi(lua_State *L, const char *consumer,
                                unsigned marker, IRType tailtype)
{
  LJArm64ABISignature abi;
  CTypeID id;
  lua_getglobal(L, consumer);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)), id, &abi));
  assert(abi.result_class == LJ_ARM64_ABI_GPR &&
         abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER &&
         abi.result_size == 4 && abi.result_unsigned);
  if (marker == UINT_MAX) {
    assert(!abi.is_vararg && abi.nargs == 2 && abi.ngpr == 1 && abi.nfpr == 1);
    assert(abi.arg[1].cls == LJ_ARM64_ABI_FPR && abi.arg[1].reg == 0 &&
           abi.arg[1].size == (tailtype == IRT_FLOAT ? 4 : 8));
  } else {
    assert(abi.is_vararg && abi.nargs == marker && abi.nfpr == 0);
    assert(abi.ngpr == (marker < 8 ? marker : 8));
    assert(abi.stack_size == (marker == 9 ? 4 : 0));
    for (unsigned i = 0; i < marker; i++)
      assert(abi.arg[i].size == 4 && abi.arg[i].cls == LJ_ARM64_ABI_GPR &&
             abi.arg[i].reg == (i < 8 ? i : LJ_ARM64_ABI_STACK));
    if (marker == 9) assert(abi.arg[8].stack_offset == 0);
  }
}

int main(void)
{
  static const char *runners[] = {
    "__u32_callm_run", "__u32_callm_double_run",
    "__u32_callm_float_run", "__u32_callm_stack_run"
  };
  static const char *consumers[] = {
    "__u32_callm_consumer", "__u32_callm_double_consumer",
    "__u32_callm_float_consumer", "__u32_callm_stack_consumer"
  };
  static const unsigned marker[] = { 1u, UINT_MAX, UINT_MAX, 9u };
  static const unsigned rawargs[] = { 5u, 2u, 2u, 13u };
  static const IRType tailtype[] = { IRT_NUM, IRT_NUM, IRT_FLOAT, IRT_NUM };
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  U32Fixture fx[4] = { {0} };
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[void lj_callxs_arm64_u32_configure(int32_t); "
    "uint32_t lj_callxs_arm64_u32(int32_t); "
    "uint32_t lj_callxs_arm64_u32_callm_consumer(int32_t,...); "
    "uint32_t lj_callxs_arm64_u32_callm_double(int32_t,double); "
    "uint32_t lj_callxs_arm64_u32_callm_float(int32_t,float); "
    "uint32_t lj_callxs_arm64_u32_callm_stack(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,...);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_U32_SO')))\n"
    "_G.__u32_callm_producer=lib.lj_callxs_arm64_u32\n"
    "_G.__u32_callm_consumer=lib.lj_callxs_arm64_u32_callm_consumer\n"
    "_G.__u32_callm_double_consumer=lib.lj_callxs_arm64_u32_callm_double\n"
    "_G.__u32_callm_float_consumer=lib.lj_callxs_arm64_u32_callm_float\n"
    "_G.__u32_callm_stack_consumer=lib.lj_callxs_arm64_u32_callm_stack\n"
    "function _G.__u32_callm_run(n) local p,c=__u32_callm_producer,__u32_callm_consumer; local sum=0.5; for i=1,n do sum=sum+c(i,i+0.25,true,p(i)) end; return sum-0.5 end\n"
    "function _G.__u32_callm_double_run(n) local p,c=__u32_callm_producer,__u32_callm_double_consumer; local sum=0.5; for i=1,n do sum=sum+c(i,p(i)) end; return sum-0.5 end\n"
    "function _G.__u32_callm_float_run(n) local p,c=__u32_callm_producer,__u32_callm_float_consumer; local sum=0.5; for i=1,n do sum=sum+c(i,p(i)) end; return sum-0.5 end\n"
    "function _G.__u32_callm_stack_run(n) local p,c=__u32_callm_producer,__u32_callm_stack_consumer; local sum=0.5; for i=1,n do sum=sum+c(i,101,102,103,104,105,106,107,108,i+0.25,true,p(i)) end; return sum-0.5 end\n"
    "function _G.__u32_callm_driver(run,n) local result=run(n); return result end\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n"
    "__u32_callm_driver(__u32_callm_run,64); __u32_callm_driver(__u32_callm_double_run,64); "
    "__u32_callm_driver(__u32_callm_float_run,64); __u32_callm_driver(__u32_callm_stack_run,64)\n");
  for (unsigned i = 0; i < 4; i++) {
    assert_consumer_abi(L, consumers[i], marker[i], tailtype[i]);
    run_callm(L, runners[i]);
    init_fixture(L, J, &fx[i], runners[i], 4, 0);
    assert_callm_pairs(&fx[i], marker[i], rawargs[i], tailtype[i]);
    for (unsigned c = 0; c < fx[i].ncalls; c++)
      assert_result_continuation(&fx[i], c);
    positive(&fx[i], J);
  }

  both(&fx[0], J, M_CALLM_RAW_TAIL);
  both(&fx[0], J, M_CALLM_WRONG_PRODUCER);
  both(&fx[0], J, M_CALLM_BAD_CONV_MODE);
  both(&fx[0], J, M_CALLM_BAD_CONV_FLAGS);
  both(&fx[0], J, M_CALLM_BAD_CONV_SOURCE);
  both(&fx[0], J, M_CALLM_BAD_MARKER);
  both(&fx[0], J, M_CALLM_DUP_MARKER);
  both(&fx[0], J, M_CALLM_BAD_PRODUCER_TYPE);
  both(&fx[0], J, M_CALLM_PRODUCER_NORESTORE);
  both(&fx[0], J, M_CALLM_PRODUCER_MISSING);
  both(&fx[0], J, M_CALLM_PRODUCER_WRONG);
  both(&fx[0], J, M_CALLM_CONSUMER_NORESTORE);
  both(&fx[0], J, M_CALLM_CONSUMER_MISSING);
  both(&fx[0], J, M_CALLM_CONSUMER_WRONG);
  both(&fx[0], J, M_CALLM_PRODUCER_BAD_PC);
  both(&fx[0], J, M_CALLM_CONSUMER_BAD_PC);
  both(&fx[0], J, M_CALLM_PRODUCER_BAD_COUNT);
  both(&fx[0], J, M_CALLM_CONSUMER_BAD_COUNT);
  both_source_mutation(&fx[0], J, 0);
  both_source_mutation(&fx[0], J, 1);
  both_source_mutation(&fx[0], J, 2);
  both_source_mutation(&fx[0], J, 3);
  both(&fx[0], J, M_CALLM_SOURCE_WRONG);
  both(&fx[1], J, M_CALLM_SOURCE_WRONG);
  both(&fx[2], J, M_CALLM_SOURCE_WRONG);
  both(&fx[3], J, M_CALLM_SOURCE_WRONG);
  both(&fx[2], J, M_CALLM_FLOAT_MODE);
  both(&fx[2], J, M_CALLM_FLOAT_FLAGS);
  both(&fx[2], J, M_CALLM_FLOAT_SOURCE);

  for (unsigned i = 0; i < 4; i++) {
    free(fx[i].ir);
    free(fx[i].snap);
    free(fx[i].snapmap);
  }
  lua_close(L);
  printf("U32 CALLM semantic=%u/%u post-RA=%u/%u\n", semantic_accepts,
         semantic_rejects, postra_accepts, postra_rejects);
  puts("t-arm64-jit-callxs-u32-callm-admission OK");
  return 0;
}
#else
int main(void) {
  puts("t-arm64-jit-callxs-u32-callm-admission SKIP");
  return 0;
}
#endif
