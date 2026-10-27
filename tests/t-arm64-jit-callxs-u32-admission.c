/* Independent semantic and post-RA admission tests for CALLXS U32 results. */

#include <assert.h>
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
#error "t-arm64-jit-callxs-u32-admission requires ARM64 CALLXS admission"
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
        "U32 semantic expected %d got %d reason=%d ref=%u op=%u detail=%u\n",
        accepted, got, (int)reject.reason, (unsigned)reject.ref,
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
  M_CTYPE,
  M_CALLTYPE,
  M_CALLWIDTH,
  M_CONVOP,
  M_CONVTYPE,
  M_CONVFLAGS,
  M_WRONG_CALL,
  M_RAW_RESULT,
  M_RAW_USE,
  M_POST_NORESTORE,
  M_POST_MISSING,
  M_POST_WRONG,
  M_LATE_CONV,
  M_AFTER_LEAVE,
  M_BAD_ALLOC,
  M_CONV_FPR,
  M_CLOBBER,
  M_SPILL_ALIGN,
  M_SPILL_EXTENT,
  M_DISCARD_COUNT,
  M_DISCARD_REF,
  M_DISCARD_PC,
  M_CHECKED_WRONG_MODE,
  M_CHECKED_NO_GUARD,
  M_CHECKED_WRONG_SOURCE,
  M_CHECKED_ACTIVE,
  M_RENAME_SCOPE,
  M_RENAME_LOCATION,
  M_RENAME_LATE,
  M_RENAME_CLOBBER
};

static IRRef find_checked_intnum(U32Fixture *fx) {
  IRRef ref;
  for (ref = REF_FIRST; ref < trace_nins_acq(fx->owner); ref++) {
    IRIns ins = ir_load_acq(&fx->ir[ref]);
    if (ins.o == IR_CONV && ins.t.irt == (IRT_INT | IRT_GUARD) &&
        ins.op2 == (IRCONV_INT_NUM | IRCONV_CHECK) && ins.op1 < ref &&
        irt_type(ir_load_acq(&fx->ir[ins.op1]).t) == IRT_NUM)
      return ref;
  }
  return 0;
}

static IRRef find_num_source_before(U32Fixture *fx, IRRef limit) {
  IRRef ref;
  for (ref = limit; ref > REF_FIRST;) {
    IRRef candidate = --ref;
    IRIns ins = ir_load_acq(&fx->ir[candidate]);
    if (irt_type(ins.t) == IRT_NUM && ins.o != IR_RENAME && ins.o != IR_PHI &&
        ins.o != IR_CALLXS)
      return candidate;
  }
  return 0;
}

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

static void swap_refs(U32Fixture *fx, IRRef a, IRRef b) {
  IRIns tmp = fx->ir[a];
  IRRef r;
  MSize s;
  fx->ir[a] = fx->ir[b];
  fx->ir[b] = tmp;
  for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
    IRIns *ins = &fx->ir[r];
    uint8_t mode = lj_ir_mode[ins->o];
    if (irm_op1(mode) == IRMref && (ins->op1 == a || ins->op1 == b))
      ins->op1 = ins->op1 == a ? b : a;
    if (irm_op2(mode) == IRMref && (ins->op2 == a || ins->op2 == b))
      ins->op2 = ins->op2 == a ? b : a;
  }
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++) {
    SnapShot *snap = &fx->snap[s];
    MSize ofs = snap_mapofs_acq(snap);
    MSize n = snap_nent_acq(snap), i;
    if (snap_ref_acq(snap) == a)
      snap->ref = b;
    else if (snap_ref_acq(snap) == b)
      snap->ref = a;
    for (i = 0; i < n; i++) {
      SnapEntry *e = &fx->snapmap[ofs + i];
      IRRef old = snap_ref(*e);
      if (old == a)
        *e = (*e & ~((SnapEntry)UINT16_MAX)) | b;
      else if (old == b)
        *e = (*e & ~((SnapEntry)UINT16_MAX)) | a;
    }
  }
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
      assert(bc_op(ins) == BC_CALL);
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

static void assert_discard_continuation(U32Fixture *fx) {
  unsigned call;
  for (call = 0; call < fx->ncalls; call++) {
    BCReg slot;
    BCIns ins = source_call(fx, call, &slot);
    MSize s;
    assert(bc_b(ins) == 1);
    for (s = 0; s < trace_nsnap_acq(fx->owner); s++) {
      if (snap_ref_acq(&fx->snap[s]) == fx->leaves[call]) {
        MSize ofs = snap_mapofs_acq(&fx->snap[s]);
        MSize n = snap_nent_acq(&fx->snap[s]), i;
        assert(snap_count_acq(&fx->snap[s]) == SNAPCOUNT_DONE);
        assert(snap_pc(&fx->snapmap[ofs + n]) == source_returnpc(fx, call));
        for (i = 0; i < n; i++)
          assert(snap_slot(fx->snapmap[ofs + i]) != slot ||
                 snap_ref(fx->snapmap[ofs + i]) != fx->calls[call]);
        break;
      }
    }
    assert(s < trace_nsnap_acq(fx->owner));
  }
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

static int snapshot_has_ref(U32Fixture *fx, MSize snapno, IRRef value);

static void assert_retained_result(U32Fixture *fx) {
  IRRef ref;
  unsigned found_phi = 0, found_rename = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(fx->owner); ref++) {
    IRIns ins = ir_load_acq(&fx->ir[ref]);
    if (ins.o == IR_PHI && irt_type(ins.t) == IRT_NUM)
      found_phi++;
  }
  assert(found_phi != 0 && "retained numeric CALLXS result should form a PHI");
  for (unsigned call = 0; call < fx->ncalls; call++) {
    BCReg slot;
    int found_leave = 0;
    (void)source_call(fx, call, &slot);
    for (MSize s = 0; s < trace_nsnap_acq(fx->owner); s++) {
      if (snap_ref_acq(&fx->snap[s]) == fx->leaves[call]) {
        MSize ofs = snap_mapofs_acq(&fx->snap[s]);
        MSize n = snap_nent_acq(&fx->snap[s]), i;
        unsigned found = 0;
        assert(snap_count_acq(&fx->snap[s]) == SNAPCOUNT_DONE);
        assert(snap_pc(&fx->snapmap[ofs + n]) == source_returnpc(fx, call));
        for (i = 0; i < n; i++)
          if (snap_slot(fx->snapmap[ofs + i]) == slot) {
            assert(snap_ref(fx->snapmap[ofs + i]) == fx->convs[call]);
            found++;
          }
        assert(found == 1);
        found_leave = 1;
        break;
      }
    }
    assert(found_leave);
  }
  /* A loop-carried result's renamed location can be needed by a later exit,
  ** after the producing CALLXS's own post-effect snapshot. */
  for (unsigned call = 0; call < fx->ncalls; call++) {
    MSize own = trace_nsnap_acq(fx->owner);
    for (MSize s = 0; s < own; s++)
      if (snap_ref_acq(&fx->snap[s]) == fx->leaves[call]) {
        own = s;
        break;
      }
    assert(own < trace_nsnap_acq(fx->owner));
    for (MSize s = own + 1u; s < trace_nsnap_acq(fx->owner); s++) {
      if (!snapshot_has_ref(fx, s, fx->convs[call]))
        continue;
      for (ref = semantic_nins(fx->owner); ref < trace_nins_acq(fx->owner);
           ref++) {
        IRIns rename = ir_load_acq(&fx->ir[ref]);
        if (rename.o == IR_RENAME && rename.op1 == fx->convs[call] &&
            rename.op2 <= s) {
          found_rename++;
          break;
        }
      }
    }
  }
  assert(found_rename != 0 && "retained result should use snapshot RENAME");
}

static void mutate_discard_continuation(U32Fixture *fx, int mutation) {
  MSize s, target = (MSize)-1;
  for (s = 0; s < trace_nsnap_acq(fx->owner); s++)
    if (snap_ref_acq(&fx->snap[s]) == fx->leaves[0]) {
      assert(target == (MSize)-1);
      target = s;
    }
  assert(target != (MSize)-1);
  if (mutation == 0) {
    assert(snap_count_acq(&fx->snap[target]) == SNAPCOUNT_DONE);
    fx->snap[target].count = 1;
    return;
  }
  if (mutation == 2) {
    MSize ofs = snap_mapofs_acq(&fx->snap[target]);
    MSize n = snap_nent_acq(&fx->snap[target]);
    uint64_t footer;
    const BCIns *wrong = source_returnpc(fx, 0) - 1;
    memcpy(&footer, &fx->snapmap[ofs + n], sizeof(footer));
    footer = (footer & UINT64_C(0xff)) | ((uint64_t)(uintptr_t)wrong << 8);
    memcpy(&fx->snapmap[ofs + n], &footer, sizeof(footer));
    return;
  }
  {
    IRRef candidate = fx->leaves[0] - 1u;
    while (candidate > REF_FIRST) {
      int ordered =
          (target == 0 || snap_ref_acq(&fx->snap[target - 1u]) <= candidate) &&
          (target + 1u >= trace_nsnap_acq(fx->owner) ||
           candidate <= snap_ref_acq(&fx->snap[target + 1u]));
      if (ordered && candidate != fx->leaves[0]) {
        fx->snap[target].ref = candidate;
        return;
      }
      candidate--;
    }
  }
  assert(!"no ordered snapshot ref available before discarded leave");
}

static int snapshot_has_ref(U32Fixture *fx, MSize snapno, IRRef value) {
  MSize ofs = snap_mapofs_acq(&fx->snap[snapno]);
  MSize n = snap_nent_acq(&fx->snap[snapno]), i;
  for (i = 0; i < n; i++)
    if (snap_ref(fx->snapmap[ofs + i]) == value)
      return 1;
  return 0;
}

static RegSP snapshot_regsp(U32Fixture *fx, IRRef value, MSize snapno) {
  RegSP rs = ir_load_acq(&fx->ir[value]).prev;
  for (IRRef r = trace_nins_acq(fx->owner); r-- > semantic_nins(fx->owner);) {
    IRIns rename = ir_load_acq(&fx->ir[r]);
    if (rename.o != IR_RENAME)
      break;
    if (rename.op1 == value && rename.op2 <= snapno)
      rs = rename.prev;
  }
  return rs;
}

static void mutate_rename(U32Fixture *fx, int mode) {
  MSize nsnap = trace_nsnap_acq(fx->owner);
  for (MSize s = 0; s < nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&fx->snap[s]);
    MSize n = snap_nent_acq(&fx->snap[s]), i;
    for (i = 0; i < n; i++) {
      IRRef value = snap_ref(fx->snapmap[ofs + i]);
      int retained = 0;
      for (unsigned call = 0; call < fx->ncalls; call++) {
        MSize own;
        if (value != fx->convs[call])
          continue;
        for (own = 0; own < nsnap; own++)
          if (snap_ref_acq(&fx->snap[own]) == fx->leaves[call])
            break;
        if (own < s) {
          retained = 1;
          break;
        }
      }
      if (!retained)
        continue;
      if (mode == 2) {
        IRIns source = ir_load_acq(&fx->ir[value]);
        RegSP default_rs = source.prev;
        RegSP current_rs = snapshot_regsp(fx, value, s);
        if (current_rs == default_rs)
          continue;
        for (MSize other_i = 0; other_i < n; other_i++) {
          IRRef other = snap_ref(fx->snapmap[ofs + other_i]);
          if (other == value || other < REF_FIRST ||
              other >= semantic_nins(fx->owner) || irref_isk(other) ||
              other >= snap_ref_acq(&fx->snap[s]))
            continue;
          if (ir_load_acq(&fx->ir[other]).prev != default_rs ||
              snapshot_regsp(fx, other, s) != default_rs)
            continue;
          /* Move every currently applicable rename for this carried value
          ** just past the colliding snapshot. The new scope remains a valid
          ** snapshot index, but the old value then falls back to the same
          ** RegSP occupied by the other live IR value. */
          assert(s + 1u < nsnap);
          for (IRRef r = semantic_nins(fx->owner);
               r < trace_nins_acq(fx->owner); r++) {
            IRIns *rename = &fx->ir[r];
            if (rename->o == IR_RENAME && rename->op1 == value &&
                rename->op2 <= s)
              rename->op2 = (uint16_t)(s + 1u);
          }
          return;
        }
        continue;
      }
      for (IRRef r = semantic_nins(fx->owner); r < trace_nins_acq(fx->owner);
           r++) {
        IRIns *rename = &fx->ir[r];
        if (rename->o != IR_RENAME || rename->op1 != value || rename->op2 > s)
          continue;
        if (mode == 1) {
          rename->prev = REGSP(RID_RET, SPS_NONE);
          return;
        }
        if (mode == 3) {
          /* D0 is a valid FPR, but CALLXS may clobber it while this carried
          ** value remains live. */
          rename->prev = REGSP(RID_D0, SPS_NONE);
          return;
        }
        if (mode == 0) {
          rename->op2 = (uint16_t)nsnap;
          return;
        }
        assert(!"unreachable late-rename mode");
      }
    }
  }
  assert(!"retained result lacks a snapshot-scoped rename mutation target");
}

/* Add a later-scope rename for an old numeric result without changing its
** D14 allocation at snapshots which still cross native CALLXS effects. New
** rename records are inserted at the head of the trailing suffix:
* snap_renameref
** scans that suffix backwards, so this earlier record wins only when its later
** snapshot scope applies. */
static MSize insert_postleave_d0_rename(U32Fixture *fx) {
  IRRef value = fx->convs[0], nins, at, renref = 0;
  MSize nsnap = trace_nsnap_acq(fx->owner), last_leave = 0, target = nsnap;
  RegSP d0 = REGSP(RID_D0, SPS_NONE);
  IRIns added;
  for (unsigned call = 0; call < fx->ncalls; call++) {
    for (MSize s = 0; s < nsnap; s++)
      if (snap_ref_acq(&fx->snap[s]) == fx->leaves[call]) {
        if (s > last_leave)
          last_leave = s;
        break;
      }
  }
  for (MSize s = last_leave + 1u; s < nsnap; s++) {
    MSize ofs, n, i;
    int free_d0 = 1;
    if (!snapshot_has_ref(fx, s, value))
      continue;
    ofs = snap_mapofs_acq(&fx->snap[s]);
    n = snap_nent_acq(&fx->snap[s]);
    for (i = 0; i < n; i++) {
      IRRef other = snap_ref(fx->snapmap[ofs + i]);
      if (other == value || other < REF_FIRST || irref_isk(other))
        continue;
      if (snapshot_regsp(fx, other, s) == d0) {
        free_d0 = 0;
        break;
      }
    }
    if (free_d0) {
      target = s;
      break;
    }
  }
  assert(target < nsnap && target > last_leave);
  for (IRRef r = semantic_nins(fx->owner); r < trace_nins_acq(fx->owner); r++) {
    IRIns rename = ir_load_acq(&fx->ir[r]);
    if (rename.o == IR_RENAME && rename.op1 == value &&
        rename.op2 <= last_leave) {
      renref = r;
      break;
    }
  }
  assert(renref != 0 && "retained result needs an earlier native-live rename");
  added = ir_load_acq(&fx->ir[renref]);
  added.op2 = (uint16_t)target;
  added.prev = d0;
  nins = trace_nins_acq(&fx->view_owner);
  at = semantic_nins(&fx->view_owner);
  while (at < nins) {
    IRIns nop = ir_load_acq(&fx->ir[at]);
    if (nop.o != IR_NOP || nop.t.irt != IRT_NIL || nop.op1 || nop.op2 ||
        nop.prev)
      break;
    at++; /* Keep reserved zero NOPs at the suffix boundary. */
  }
  assert(at < nins && fx->ir[at].o == IR_RENAME);
  assert(nins < trace_nins_acq(fx->owner) + 1u);
  /* The suffix's RENAME operands and all snapshot refs point below at, so
  ** inserting here leaves their IR references and map geometry unchanged. */
  for (MSize s = 0; s < nsnap; s++)
    assert(snap_ref_acq(&fx->snap[s]) < at);
  memmove(&fx->ir[at + 1u], &fx->ir[at], (size_t)(nins - at) * sizeof(IRIns));
  fx->ir[at] = added;
  fx->view_owner.nins = nins + 1u;
  return target;
}

static void mutate(U32Fixture *fx, enum Mutation m) {
  unsigned ix = fx->ncalls - 1u, other = 0;
  IRRef call = fx->calls[ix], conv = fx->convs[ix], leave = fx->leaves[ix];
  switch (m) {
  case M_CTYPE: {
    IRRef r;
    int found = 0;
    for (r = REF_FIRST; r < call && !found; r++) {
      IRIns f = ir_load_acq(&fx->ir[r]);
      if (f.o != IR_FLOAD || f.op2 != IRFL_CDATA_CTYPEID)
        continue;
      for (IRRef g = r + 1u; g < call; g++) {
        IRIns eq = ir_load_acq(&fx->ir[g]);
        IRRef k;
        if (eq.o != IR_EQ || !irt_isguard(eq.t) || (eq.op1 != r && eq.op2 != r))
          continue;
        k = eq.op1 == r ? eq.op2 : eq.op1;
        if (k >= trace_nk_acq(fx->owner) && k < REF_TRUE) {
          fx->ir[k].i = CTID_INT32;
          found = 1;
        }
        break;
      }
    }
    assert(found);
    break;
  }
  case M_CALLTYPE:
    fx->ir[call].t.irt = IRT_INT;
    break;
  case M_CALLWIDTH:
    fx->ir[call].t.irt = IRT_U64;
    break;
  case M_CONVOP:
    fx->ir[conv].op2 = IRCONV_NUM_INT;
    break;
  case M_CONVTYPE:
    fx->ir[conv].t.irt = IRT_INT | IRT_ISPHI;
    break;
  case M_CONVFLAGS:
    fx->ir[conv].t.irt = IRT_NUM | IRT_GUARD;
    break;
  case M_WRONG_CALL:
    assert(fx->ncalls >= 2);
    other = 0;
    fx->ir[conv].op1 = fx->calls[other];
    break;
  case M_RAW_RESULT: {
    MSize s;
    int done = 0;
    BCReg slot;
    (void)source_call(fx, ix, &slot);
    for (s = 0; s < trace_nsnap_acq(fx->owner) && !done; s++) {
      MSize ofs = snap_mapofs_acq(&fx->snap[s]),
            n = snap_nent_acq(&fx->snap[s]), i;
      if (snap_ref_acq(&fx->snap[s]) != leave)
        continue;
      for (i = 0; i < n; i++)
        if (snap_slot(fx->snapmap[ofs + i]) == slot &&
            snap_ref(fx->snapmap[ofs + i]) == conv) {
          SnapEntry e = fx->snapmap[ofs + i];
          fx->snapmap[ofs + i] = (e & ~((SnapEntry)UINT16_MAX)) | call;
          done = 1;
          break;
        }
    }
    assert(done);
    break;
  }
  case M_RAW_USE: {
    IRRef r;
    int changed = 0;
    for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
      IRIns *use = &fx->ir[r];
      uint8_t mode = lj_ir_mode[use->o];
      if (r == conv || r == leave)
        continue;
      if (irm_op1(mode) == IRMref && use->op1 == conv) {
        use->op1 = call;
        changed = 1;
        break;
      }
      if (irm_op2(mode) == IRMref && use->op2 == conv) {
        use->op2 = call;
        changed = 1;
        break;
      }
    }
    assert(changed && "numeric loop should consume the converted result");
    break;
  }
  case M_POST_NORESTORE:
  case M_POST_MISSING:
  case M_POST_WRONG: {
    BCReg slot;
    IRRef at = leave;
    (void)source_call(fx, ix, &slot);
    if (m == M_POST_WRONG) {
      assert(fx->ncalls >= 2);
      change_slot(fx, at, slot, fx->convs[other], 0, 0);
    } else if (m == M_POST_NORESTORE)
      change_slot(fx, at, slot, fx->convs[ix], SNAP_NORESTORE, 0);
    else
      change_slot(fx, at, slot, REF_NIL, 0, 1);
    break;
  }
  case M_LATE_CONV:
    /* A forward source proves that the accepted conversion cannot precede
    ** its CALLXS even if its type/mode otherwise look right. */
    fx->ir[conv].op1 = (IRRef)(conv + 1u);
    break;
  case M_AFTER_LEAVE:
    assert(conv < leave);
    swap_refs(fx, conv, leave);
    break;
  case M_CONV_FPR:
    fx->ir[conv].r = RID_RET;
    fx->ir[conv].s = SPS_NONE;
    break;
  case M_CLOBBER:
    /* The converted number is consumed after the native leave helper. */
    fx->ir[conv].r = RID_FPRET;
    fx->ir[conv].s = SPS_NONE;
    break;
  case M_SPILL_ALIGN:
    fx->ir[conv].r = RID_NONE;
    fx->ir[conv].s = SPS_FIRST + 1u;
    break;
  case M_SPILL_EXTENT:
    fx->ir[conv].r = RID_NONE;
    fx->ir[conv].s = SPS_LIMIT - 2u;
    break;
  case M_DISCARD_COUNT:
    assert(fx->discarded);
    mutate_discard_continuation(fx, 0);
    break;
  case M_DISCARD_REF:
    assert(fx->discarded);
    mutate_discard_continuation(fx, 1);
    break;
  case M_DISCARD_PC:
    assert(fx->discarded);
    mutate_discard_continuation(fx, 2);
    break;
  case M_CHECKED_WRONG_MODE:
  case M_CHECKED_NO_GUARD:
  case M_CHECKED_WRONG_SOURCE: {
    IRRef checked = find_checked_intnum(fx);
    assert(checked != 0);
    if (m == M_CHECKED_WRONG_MODE)
      fx->ir[checked].op2 = IRCONV_INT_NUM;
    else if (m == M_CHECKED_NO_GUARD)
      fx->ir[checked].t.irt = IRT_INT;
    else {
      IRRef bad = 0;
      for (unsigned i = 0; i < fx->ncalls; i++)
        if (fx->calls[i] < checked) {
          bad = fx->calls[i];
          break;
        }
      assert(bad != 0);
      fx->ir[checked].op1 = bad;
    }
    break;
  }
  case M_CHECKED_ACTIVE: {
    IRRef checked = find_checked_intnum(fx);
    IRIns sample;
    assert(fx->ncalls >= 2 && checked != 0);
    sample = ir_load_acq(&fx->ir[checked]);
    for (unsigned i = 0; i < fx->ncalls; i++) {
      IRRef callref = fx->calls[i], convref = fx->convs[i];
      IRRef source = sample.op1 < callref ? sample.op1
                                          : find_num_source_before(fx, callref);
      if (source != 0 && convref > callref && convref < fx->leaves[i]) {
        fx->ir[convref].t = sample.t;
        fx->ir[convref].op1 = source;
        fx->ir[convref].op2 = sample.op2;
        return;
      }
    }
    assert(!"no active U32 result conversion with an earlier NUM source");
    break;
  }
  case M_RENAME_SCOPE:
    mutate_rename(fx, 0);
    break;
  case M_RENAME_LOCATION:
    mutate_rename(fx, 1);
    break;
  case M_RENAME_LATE:
    mutate_rename(fx, 2);
    break;
  case M_RENAME_CLOBBER:
    mutate_rename(fx, 3);
    break;
  case M_BAD_ALLOC:
    fx->ir[call].r = RID_FPRET;
    break;
  }
}

static void both(U32Fixture *fx, jit_State *J, enum Mutation m) {
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize idx = (MSize)((start - base) / sizeof(BCIns));
  BCIns saved = loadbc(&bc[idx]);
  JSave js;
  active_mutation = (int)m;
  assert(idx < fx->pt->sizebc && bc_op(saved) == BC_JFORL);
  if (m == M_BAD_ALLOC || m == M_CONV_FPR || m == M_CLOBBER ||
      m == M_SPILL_ALIGN || m == M_SPILL_EXTENT || m == M_RENAME_SCOPE ||
      m == M_RENAME_LOCATION || m == M_RENAME_LATE || m == M_RENAME_CLOBBER) {
    reset_fixture(fx);
    mutate(fx, m);
    {
      LJArm64PostRAView v = postra_view(fx);
      expect_postra(&v, 0);
    }
    return;
  }
  reset_fixture(fx);
  mutate(fx, m);
  bc_publish((const uint32_t *)&bc[idx], trace_startins_acq(fx->owner));
  js = enter_semantic(J, fx);
  expect_semantic(J, fx, 0);
  leave_semantic(J, &js);
  bc_publish((const uint32_t *)&bc[idx], saved);
  reset_fixture(fx);
  mutate(fx, m);
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

static void run(lua_State *L, const char *runner, const char *fn) {
  lua_getglobal(L, "__u32_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, fn);
  lua_pushinteger(L, 64);
  if (lua_pcall(L, 3, 1, 0) != 0) {
    fprintf(stderr, "U32 runner failed: %s\n", lua_tostring(L, -1));
    abort();
  }
  lua_pop(L, 1);
}

int main(void) {
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  U32Fixture single = {0}, pair = {0}, discard = {0}, retained = {0};
  ljt_lua_dostring(
      L, "local ffi=require('ffi')\n"
         "ffi.cdef[[void lj_callxs_arm64_u32_configure(int32_t); uint32_t "
         "lj_callxs_arm64_u32(int32_t); uint32_t "
         "lj_callxs_arm64_u32_alt(int32_t);]]\n"
         "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_U32_SO')))\n"
         "local set_errno=function() end\n"
         "_G.__u32_call=lib.lj_callxs_arm64_u32; "
         "_G.__u32_alt_call=lib.lj_callxs_arm64_u32_alt\n"
         "function _G.__u32_run(fn,n) local s=0; for i=1,n do s=s+fn(i) end; "
         "return s end\n"
         "function _G.__u32_pair_run(fn,n) local alt=__u32_alt_call; local "
         "a,b=0,0; for i=1,n do "
         "a=a+fn(i); b=b+alt(i) end; return 2*a+5*b end\n"
         "function _G.__u32_discard_run(fn,n) for i=1,n do fn(i) end; return n "
         "end\n"
         "function _G.__u32_retained_run(fn,n) local result=0; for i=1,n do "
         "result=fn(0) end; return result end\n"
         "function _G.__u32_driver(run,fn,n) local r=run(fn,n); return r end\n"
         "jit.on(); jit.flush(); "
         "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n"
         "__u32_driver(__u32_run,__u32_call,64); "
         "__u32_driver(__u32_pair_run,__u32_call,64); "
         "__u32_driver(__u32_discard_run,__u32_call,64); "
         "__u32_driver(__u32_retained_run,__u32_call,64)\n");
  run(L, "__u32_run", "__u32_call");
  run(L, "__u32_pair_run", "__u32_call");
  run(L, "__u32_discard_run", "__u32_call");
  run(L, "__u32_retained_run", "__u32_call");
  init_fixture(L, J, &single, "__u32_run", 2, 0);
  init_fixture(L, J, &pair, "__u32_pair_run", 4, 0);
  init_fixture(L, J, &discard, "__u32_discard_run", 2, 1);
  init_fixture(L, J, &retained, "__u32_retained_run", 2, 0);
  for (unsigned i = 0; i < single.ncalls; i++)
    assert_result_continuation(&single, i);
  for (unsigned i = 0; i < pair.ncalls; i++)
    assert_result_continuation(&pair, i);
  assert_discard_continuation(&discard);
  assert_retained_result(&retained);
  positive(&single, J);
  positive(&pair, J);
  positive(&discard, J);
  positive(&retained, J);
  reset_fixture(&retained);
  (void)insert_postleave_d0_rename(&retained);
  active_mutation = -2;
  {
    LJArm64PostRAView v = postra_view(&retained);
    expect_postra(&v, 1);
  }
  both(&single, J, M_CTYPE);
  both(&single, J, M_CALLTYPE);
  both(&single, J, M_CALLWIDTH);
  both(&single, J, M_CONVOP);
  both(&single, J, M_CONVTYPE);
  both(&single, J, M_CONVFLAGS);
  both(&pair, J, M_WRONG_CALL);
  both(&single, J, M_RAW_RESULT);
  both(&single, J, M_RAW_USE);
  both(&single, J, M_POST_NORESTORE);
  both(&single, J, M_POST_MISSING);
  both(&pair, J, M_POST_WRONG);
  both(&single, J, M_LATE_CONV);
  both(&single, J, M_AFTER_LEAVE);
  both(&single, J, M_BAD_ALLOC);
  both(&single, J, M_CONV_FPR);
  both(&single, J, M_CLOBBER);
  both(&single, J, M_SPILL_ALIGN);
  both(&single, J, M_SPILL_EXTENT);
  both(&discard, J, M_DISCARD_COUNT);
  both(&discard, J, M_DISCARD_REF);
  both(&discard, J, M_DISCARD_PC);
  both(&single, J, M_CHECKED_WRONG_MODE);
  both(&single, J, M_CHECKED_NO_GUARD);
  both(&single, J, M_CHECKED_WRONG_SOURCE);
  both(&pair, J, M_CHECKED_ACTIVE);
  both(&retained, J, M_RENAME_SCOPE);
  both(&retained, J, M_RENAME_LOCATION);
  both(&retained, J, M_RENAME_LATE);
  both(&retained, J, M_RENAME_CLOBBER);
  free(single.ir);
  free(single.snap);
  free(single.snapmap);
  free(pair.ir);
  free(pair.snap);
  free(pair.snapmap);
  free(discard.ir);
  free(discard.snap);
  free(discard.snapmap);
  free(retained.ir);
  free(retained.snap);
  free(retained.snapmap);
  lua_close(L);
  printf("U32 CALLXS semantic=%u/%u post-RA=%u/%u\n", semantic_accepts,
         semantic_rejects, postra_accepts, postra_rejects);
  puts("t-arm64-jit-callxs-u32-admission OK");
  return 0;
}

#else
int main(void) {
  puts("t-arm64-jit-callxs-u32-admission SKIP");
  return 0;
}
#endif
