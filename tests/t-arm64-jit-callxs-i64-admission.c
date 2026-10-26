/* Independent semantic and post-RA mutation tests for boxed I64/U64 CALLXS. */

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

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-i64-admission requires ARM64 CALLXS admission"
#endif

typedef struct I64Fixture {
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef semantic_nins;
  IRRef ktrace;
  IRRef box[8];
  IRRef store[8];
  IRRef call[8];
  IRRef xsave[8];
  IRRef leave[8];
  unsigned nbox;
  GCtrace semantic;
} I64Fixture;

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

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

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
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
    if (ir_load_acq(&ir[ref]).o == op) n++;
  return n;
}

/* Keep this reader fixture-local: the admission implementation is static and
** the test must not inherit its source acceptance logic. */
static int fixture_kint(const IRIns *ir, IRRef nk, IRRef target, int32_t *valuep)
{
  IRRef ref;
  for (ref = nk; ref < REF_TRUE; ) {
    IRIns k = ir_load_acq(&ir[ref]);
    if (ref == target) {
      if (k.o != IR_KINT || k.t.irt != IRT_INT) return 0;
      if (valuep) *valuep = k.i;
      return 1;
    }
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR))) {
      if (ref+1u >= REF_TRUE) return 0;
      ref += 2u;
    } else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
               k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL) {
      ref++;
    } else {
      return 0;
    }
  }
  return 0;
}

static IRRef find_owner_ktrace(const GCtrace *T)
{
  IRRef ref, nk = trace_nk_acq((GCtrace *)T);
  const IRIns *ir = trace_ir_acq(T);
  for (ref = nk; ref < REF_TRUE; ) {
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

static IRRef find_loopref(const GCtrace *T)
{
  IRRef ref;
  const IRIns *ir = trace_ir_acq(T);
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
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

static GCtrace *find_trace(jit_State *J, GCproto *pt, int callm)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    unsigned xsaves, calls, boxes, stores;
    if (T == NULL || !trace_runnable_acq(T, no) ||
        trace_root_acq(T) != 0 || trace_startpt_acq(T) != pt ||
        bc_op(trace_startins_acq(T)) != BC_FORL)
      continue;
    xsaves = op_count(T, IR_XSAVE);
    calls = op_count(T, IR_CALLXS);
    boxes = op_count(T, IR_CNEW);
    stores = op_count(T, IR_XSTORE);
    if (callm ? (xsaves == 4 && calls == 4 && boxes == 2 && stores == 2) :
                (xsaves == 2 && calls == 2 && boxes == 2 && stores == 2))
      return T;
  }
  return NULL;
}

static void locate_boxed_calls(I64Fixture *fx)
{
  IRRef ref, nins = trace_nins_acq(fx->owner);
  fx->nbox = 0;
  for (ref = REF_FIRST; ref < nins; ref++) {
    IRIns box = ir_load_acq(&fx->ir[ref]);
    IRRef s;
    int32_t cid;
    if (box.o != IR_CNEW || irt_type(box.t) != IRT_CDATA ||
        box.op1 < trace_nk_acq(fx->owner) ||
        !fixture_kint(fx->ir, trace_nk_acq(fx->owner), box.op1, &cid))
      continue;
    for (s = REF_FIRST; s < nins; s++) {
      IRIns store = ir_load_acq(&fx->ir[s]);
      IRIns payload;
      if (store.o != IR_XSTORE || store.op1 < REF_FIRST ||
          store.op1 >= s || store.op2 < REF_FIRST || store.op2 >= s)
        continue;
      payload = ir_load_acq(&fx->ir[store.op1]);
      if (payload.o == IR_ADD && payload.op1 == ref &&
          fx->nbox < sizeof(fx->box)/sizeof(fx->box[0])) {
        IRRef x, leave = 0;
        fx->box[fx->nbox] = ref;
        fx->store[fx->nbox] = s;
        fx->call[fx->nbox] = store.op2;
        for (x = s; x > REF_FIRST; ) {
          IRIns prev = ir_load_acq(&fx->ir[--x]);
          if (prev.o == IR_XSAVE) {
            fx->xsave[fx->nbox] = x;
            break;
          }
        }
        for (x = store.op2+1u; x < nins; x++) {
          IRIns next = ir_load_acq(&fx->ir[x]);
          if (next.o == IR_CALLS &&
              next.op2 == IRCALL_lj_ffi_native_trace_leave) {
            leave = x;
            break;
          }
        }
        fx->leave[fx->nbox] = leave;
        assert(fx->xsave[fx->nbox] != 0 && leave != 0);
        fx->nbox++;
        break;
      }
    }
  }
  assert(fx->nbox >= 2);
  assert(fx->box[0] < fx->box[1] && fx->call[0] < fx->call[1]);
}

static void init_fixture(lua_State *L, jit_State *J, I64Fixture *fx,
                         const char *runner, int callm)
{
  TraceNo no;
  fx->pt = global_proto(L, runner);
  fx->owner = find_trace(J, fx->pt, callm);
  if (fx->owner == NULL)
    fprintf(stderr, "boxed CALLXS trace missing for %s\n", runner);
  assert(fx->owner != NULL);
  fx->cts = ctype_ctsG(G(L));
  fx->semantic_nins = semantic_nins(fx->owner);
  fx->ktrace = find_owner_ktrace(fx->owner);
  assert(fx->cts != NULL && fx->ktrace != 0 && find_loopref(fx->owner) != 0);
  fx->ir = (IRIns *)calloc((size_t)trace_nins_acq(fx->owner), sizeof(IRIns));
  fx->snap = (SnapShot *)calloc(trace_nsnap_acq(fx->owner), sizeof(SnapShot));
  fx->snapmap = (SnapEntry *)calloc(trace_nsnapmap_acq(fx->owner), sizeof(SnapEntry));
  assert(fx->ir != NULL && fx->snap != NULL && fx->snapmap != NULL);
  no = trace_nk_acq(fx->owner);
  memset(fx->ir, 0, (size_t)trace_nins_acq(fx->owner)*sizeof(IRIns));
  memcpy(&fx->ir[trace_nk_acq(fx->owner)],
         &trace_ir_acq(fx->owner)[trace_nk_acq(fx->owner)],
         (size_t)(trace_nins_acq(fx->owner)-trace_nk_acq(fx->owner))*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)trace_nsnap_acq(fx->owner)*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)trace_nsnapmap_acq(fx->owner)*sizeof(SnapEntry));
  assert(no == trace_nk_acq(fx->owner));
  locate_boxed_calls(fx);
}

static void reset_clone(I64Fixture *fx)
{
  memset(fx->ir, 0, (size_t)trace_nins_acq(fx->owner)*sizeof(IRIns));
  memcpy(&fx->ir[trace_nk_acq(fx->owner)],
         &trace_ir_acq(fx->owner)[trace_nk_acq(fx->owner)],
         (size_t)(trace_nins_acq(fx->owner)-trace_nk_acq(fx->owner))*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)trace_nsnap_acq(fx->owner)*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)trace_nsnapmap_acq(fx->owner)*sizeof(SnapEntry));
}

static JSave enter_semantic(jit_State *J, I64Fixture *fx)
{
  JSave save = { J->curfinal, J->pt, J->startpc, J->loopref, J->parent,
    J->exitno, J->baseslot, J->framedepth, J->retdepth, J->ktrace };
  fx->semantic = *fx->owner;
  fx->semantic.ir = fx->ir;
  fx->semantic.snap = fx->snap;
  fx->semantic.snapmap = fx->snapmap;
  fx->semantic.nins = fx->semantic_nins;
  fx->ir[fx->ktrace].o = IR_KNUM;
  fx->ir[fx->ktrace].t.irt = IRT_P64;
  fx->ir[fx->ktrace].op12 = 0;
  fx->ir[fx->ktrace].prev = 0;
  fx->ir[fx->ktrace+1u].tv.u64 = 0;
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = trace_startpc_acq(fx->owner);
  J->loopref = find_loopref(fx->owner);
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

static LJArm64PostRAView postra_view(I64Fixture *fx)
{
  LJArm64PostRAView v = { 0 };
  v.ir = fx->ir;
  v.owner = fx->owner;
  v.cts = fx->cts;
  v.snap = fx->snap;
  v.snapmap = fx->snapmap;
  v.proto_bc = proto_bc(fx->pt);
  v.proto = fx->pt;
  v.startpc = trace_startpc_acq(fx->owner);
  v.nins = trace_nins_acq(fx->owner);
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

static void expect_semantic(jit_State *J, I64Fixture *fx, int admitted)
{
  LJArm64IRReject reject;
  int got = lj_asm_arm64_ir_admit(J, &fx->semantic, &reject);
  if (got != admitted)
    fprintf(stderr, "boxed CALLXS semantic expected %d, got %d: reason=%d ref=%u op=%u detail=%u\n",
            admitted, got, (int)reject.reason, (unsigned)reject.ref,
            (unsigned)reject.op, (unsigned)reject.detail);
  assert(got == admitted);
}

static void expect_postra(LJArm64PostRAView *v, int admitted)
{
  IRRef semantic = 0;
  int got = lj_asm_arm64_postra_admit(v, &semantic);
  if (got != admitted)
    fprintf(stderr, "boxed CALLXS post-RA expected %d, got %d\n", admitted, got);
  assert(got == admitted);
  if (admitted) assert(semantic > REF_FIRST && semantic <= v->nins);
}

enum Mutation {
  MUT_CTYPE, MUT_CALL_TYPE, MUT_STORE_TYPE, MUT_PAIRED_UNSIGNED,
  MUT_CROSS_WIDTH, MUT_OFFSET, MUT_BASE, MUT_STORE_CALL, MUT_ENTRY_ROOT,
  MUT_REMOVE_STORE,
  MUT_PHI_INPUT, MUT_PRE_SNAPSHOT_OTHER, MUT_POST_SNAPSHOT_OTHER,
  MUT_PRE_SNAPSHOT_MISSING, MUT_POST_SNAPSHOT_MISSING,
  MUT_PRE_SNAPSHOT_NORESTORE, MUT_POST_SNAPSHOT_NORESTORE,
  MUT_DUP_STORE, MUT_VARARG_SEPARATOR
};

static unsigned choose_box(int alternate)
{
  return alternate ? 1u : 0u;
}

static void mutate(I64Fixture *fx, enum Mutation m, int unsigned_result)
{
  unsigned ix = choose_box(1), other = choose_box(0);
  IRRef box = fx->box[ix], store = fx->store[ix], call = fx->call[ix];
  IRIns payload = ir_load_acq(&fx->ir[ir_load_acq(&fx->ir[store]).op1]);
  IRType other_type = unsigned_result ? IRT_I64 : IRT_U64;
  switch (m) {
  case MUT_CTYPE:
    fx->ir[fx->ir[box].op1].i = unsigned_result ? CTID_INT64 : CTID_UINT64;
    break;
  case MUT_CALL_TYPE:
    fx->ir[call].t.irt = other_type;
    break;
  case MUT_STORE_TYPE:
    fx->ir[store].t.irt = other_type;
    break;
  case MUT_PAIRED_UNSIGNED:
    fx->ir[call].t.irt = other_type;
    fx->ir[store].t.irt = other_type;
    break;
  case MUT_CROSS_WIDTH:
    fx->ir[call].t.irt = IRT_INT;
    fx->ir[store].t.irt = IRT_INT;
    break;
  case MUT_OFFSET:
    assert(payload.o == IR_ADD && payload.op2 >= trace_nk_acq(fx->owner));
    fx->ir[payload.op2+1u].tv.u64 ^= UINT64_C(1);
    break;
  case MUT_BASE:
    assert(fx->box[ix] != fx->box[other]);
    fx->ir[ir_load_acq(&fx->ir[store]).op1].op1 = fx->box[other];
    break;
  case MUT_STORE_CALL:
    fx->ir[store].op2 = fx->call[other];
    break;
  case MUT_ENTRY_ROOT: {
    IRRef r;
    int changed = 0;
    for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
      IRIns enter = ir_load_acq(&fx->ir[r]);
      IRRef carg;
      if (enter.o != IR_CALLS || enter.op2 != IRCALL_lj_ffi_native_trace_enter)
        continue;
      carg = enter.op1;
      while (carg >= REF_FIRST && carg < r) {
        IRIns arg = ir_load_acq(&fx->ir[carg]);
        if (arg.o != IR_CARG) break;
        if (arg.op2 == box) {
          fx->ir[carg].op2 = fx->box[other];
          changed = 1;
          break;
        }
        carg = arg.op1;
      }
      if (changed) break;
    }
    assert(changed && "native entry did not carry the boxed result root");
    break;
  }
  case MUT_REMOVE_STORE:
    fx->ir[store].o = IR_NOP;
    fx->ir[store].t.irt = IRT_NIL;
    fx->ir[store].op1 = fx->ir[store].op2 = 0;
    fx->ir[store].prev = 0;
    break;
  case MUT_PHI_INPUT: {
    IRRef r;
    int changed = 0;
    for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
      IRIns phi = ir_load_acq(&fx->ir[r]);
      if (phi.o == IR_PHI && irt_type(phi.t) == IRT_CDATA &&
          (phi.op1 == fx->box[0] || phi.op2 == fx->box[0] ||
           phi.op1 == fx->box[1] || phi.op2 == fx->box[1])) {
        if (phi.op1 == fx->box[0] || phi.op1 == fx->box[1])
          fx->ir[r].op1 = call;
        else
          fx->ir[r].op2 = call;
        changed = 1;
        break;
      }
    }
    assert(changed && "trace lacks boxed CData loop PHI");
    break;
  }
  case MUT_PRE_SNAPSHOT_OTHER:
  case MUT_POST_SNAPSHOT_OTHER:
  case MUT_PRE_SNAPSHOT_MISSING:
  case MUT_POST_SNAPSHOT_MISSING:
  case MUT_PRE_SNAPSHOT_NORESTORE:
  case MUT_POST_SNAPSHOT_NORESTORE: {
    MSize s, i;
    int changed = 0;
    for (s = 0; s < trace_nsnap_acq(fx->owner) && !changed; s++) {
      MSize ofs = snap_mapofs_acq(&fx->snap[s]);
      MSize nent = snap_nent_acq(&fx->snap[s]);
      IRRef at = snap_ref_acq(&fx->snap[s]);
      int pre = m == MUT_PRE_SNAPSHOT_OTHER ||
                m == MUT_PRE_SNAPSHOT_MISSING ||
                m == MUT_PRE_SNAPSHOT_NORESTORE;
      if (at != (pre ? fx->xsave[ix] : fx->leave[ix])) continue;
      for (i = 0; i < nent; i++) {
        SnapEntry se = fx->snapmap[ofs+i];
        if (snap_ref(se) != box) continue;
        if (m == MUT_PRE_SNAPSHOT_OTHER || m == MUT_POST_SNAPSHOT_OTHER)
          fx->snapmap[ofs+i] = (se & ~(SnapEntry)UINT16_MAX) | fx->box[other];
        else if (m == MUT_PRE_SNAPSHOT_NORESTORE ||
                 m == MUT_POST_SNAPSHOT_NORESTORE)
          fx->snapmap[ofs+i] = se | SNAP_NORESTORE;
        else
          fx->snapmap[ofs+i] = (se & ~(SnapEntry)UINT16_MAX) | REF_NIL;
        changed = 1;
        break;
      }
    }
    assert(changed && "expected boxed result in exact pre/post snapshot");
    break;
  }
  case MUT_DUP_STORE: {
    IRRef r, end = trace_nins_acq(fx->owner);
    int changed = 0;
    for (r = store+1u; r < end; r++) {
      IRIns ins = ir_load_acq(&fx->ir[r]);
      if (ins.o == IR_CALLS && ins.op2 == IRCALL_lj_ffi_native_trace_leave)
        break;
      if (ins.o == IR_NOP && ins.t.irt == IRT_NIL &&
          ins.op1 == 0 && ins.op2 == 0 && ins.prev == 0) {
        fx->ir[r] = fx->ir[store];
        changed = 1;
        break;
      }
    }
    assert(changed && "no spare NOP in call interval for duplicate-store mutation");
    break;
  }
  case MUT_VARARG_SEPARATOR: {
    IRRef r, alternate = 0;
    int changed = 0;
    for (r = REF_FIRST; r < call; r++) {
      IRIns carg = ir_load_acq(&fx->ir[r]);
      if (carg.o == IR_CARG && carg.op2 != REF_NIL) {
        alternate = carg.op2;
        break;
      }
    }
    assert(alternate != 0);
    for (r = REF_FIRST; r < call; r++) {
      IRIns carg = ir_load_acq(&fx->ir[r]);
      if (carg.o == IR_CARG && carg.op2 == REF_NIL) {
        fx->ir[r].op2 = alternate;
        changed = 1;
        break;
      }
    }
    assert(changed && "variadic trace lacks NIL separator");
    break;
  }
  }
}

static void test_semantic_mutation(jit_State *J, I64Fixture *fx,
                                   enum Mutation m, int is_unsigned)
{
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize index = (MSize)((start-base)/sizeof(BCIns));
  BCIns saved = loadbc(&bc[index]);
  JSave save;
  assert(index < fx->pt->sizebc && bc_op(saved) == BC_JFORL);
  bc_publish((const uint32_t *)&bc[index], trace_startins_acq(fx->owner));
  reset_clone(fx);
  save = enter_semantic(J, fx);
  /* Re-entering also rebuilds the KTRACE payload after reset_clone. */
  mutate(fx, m, is_unsigned);
  expect_semantic(J, fx, 0);
  leave_semantic(J, &save);
  bc_publish((const uint32_t *)&bc[index], saved);
}

static void test_postra_mutation(I64Fixture *fx, enum Mutation m,
                                 int is_unsigned)
{
  LJArm64PostRAView v;
  reset_clone(fx);
  v = postra_view(fx);
  mutate(fx, m, is_unsigned);
  expect_postra(&v, 0);
}

static void expect_both(I64Fixture *fx, jit_State *J, enum Mutation m,
                        int is_unsigned)
{
  test_semantic_mutation(J, fx, m, is_unsigned);
  test_postra_mutation(fx, m, is_unsigned);
}

static int has_spare_call_nop(const I64Fixture *fx)
{
  IRRef r, store = fx->store[1];
  for (r = store+1u; r < trace_nins_acq(fx->owner); r++) {
    IRIns ins = ir_load_acq(&fx->ir[r]);
    if (ins.o == IR_CALLS && ins.op2 == IRCALL_lj_ffi_native_trace_leave)
      return 0;
    if (ins.o == IR_NOP && ins.t.irt == IRT_NIL &&
        ins.op1 == 0 && ins.op2 == 0 && ins.prev == 0)
      return 1;
  }
  return 0;
}

static int run_loop(lua_State *L, const char *runner, const char *func)
{
  lua_getglobal(L, "__i64_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, func);
  lua_pushinteger(L, 64);
  if (lua_pcall(L, 3, 1, 0) != 0) {
    fprintf(stderr, "runner %s failed: %s\n", runner, lua_tostring(L, -1));
    return 0;
  }
  lua_pop(L, 1);
  return 1;
}

static int run_callm(lua_State *L)
{
  lua_getglobal(L, "__i64_callm_driver");
  lua_pushinteger(L, 64);
  if (lua_pcall(L, 1, 1, 0) != 0) {
    fprintf(stderr, "CALLM runner failed: %s\n", lua_tostring(L, -1));
    return 0;
  }
  lua_pop(L, 1);
  return 1;
}

static void assert_positive(I64Fixture *fx, jit_State *J)
{
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize index = (MSize)((start-base)/sizeof(BCIns));
  BCIns saved;
  JSave save;
  LJArm64PostRAView v;
  assert(index < fx->pt->sizebc);
  saved = loadbc(&bc[index]);
  assert(bc_op(saved) == BC_JFORL);
  bc_publish((const uint32_t *)&bc[index], trace_startins_acq(fx->owner));
  reset_clone(fx);
  save = enter_semantic(J, fx);
  expect_semantic(J, fx, 1);
  leave_semantic(J, &save);
  bc_publish((const uint32_t *)&bc[index], saved);
  reset_clone(fx);
  v = postra_view(fx);
  expect_postra(&v, 1);
}

int main(void)
{
  lua_State *L;
  jit_State *J;
  I64Fixture fx[5] = { 0 };
  const char *runners[5] = { "__i64_run", "__u64_run",
    "__i64_vararg_run", "__u64_vararg_run", "__i64_callm_run" };
  const char *functions[4] = { "__i64_i64_call", "__i64_u64_call",
    "__i64_i64_vararg", "__i64_u64_vararg" };
  unsigned i;

  L = ljt_lua_newstate_openlibs();
  J = G2J(G(L));
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_arm64_i64_configure(int32_t);\n"
    "int64_t lj_callxs_arm64_i64(int32_t);\n"
    "uint64_t lj_callxs_arm64_u64(int32_t);\n"
    "int64_t lj_callxs_arm64_i64_vararg(int32_t, ...);\n"
    "uint64_t lj_callxs_arm64_u64_vararg(int32_t, ...);\n"
    "double lj_callxs_arm64_i64_callm_producer(int32_t);\n"
    "int64_t lj_callxs_arm64_i64_callm(int32_t, ...);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_I64_SO')))\n"
    "_G.__i64_i64_call=lib.lj_callxs_arm64_i64\n"
    "_G.__i64_u64_call=lib.lj_callxs_arm64_u64\n"
    "_G.__i64_i64_vararg=lib.lj_callxs_arm64_i64_vararg\n"
    "_G.__i64_u64_vararg=lib.lj_callxs_arm64_u64_vararg\n"
    "_G.__i64_callm_producer=lib.lj_callxs_arm64_i64_callm_producer\n"
    "_G.__i64_callm_consumer=lib.lj_callxs_arm64_i64_callm\n"
    "function _G.__i64_run(fn,n) local r; for i=1,n do r=fn(i) end; return r end\n"
    "function _G.__u64_run(fn,n) local r; for i=1,n do r=fn(i) end; return r end\n"
    "function _G.__i64_vararg_run(fn,n) local r; for i=1,n do r=fn(i,i+0.5,true,'i64-vararg') end; return r end\n"
    "function _G.__u64_vararg_run(fn,n) local r; for i=1,n do r=fn(i,i+0.5,true,'u64-vararg') end; return r end\n"
    "function _G.__i64_callm_run(n) local r; local p,c=__i64_callm_producer,__i64_callm_consumer; for i=1,n do r=c(17,p(i)) end; return r end\n"
    "function _G.__i64_driver(run,fn,n) local r=run(fn,n); return r end\n"
    "function _G.__i64_callm_driver(n) local r=__i64_callm_run(n); return r end\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n"
    "local a=__i64_driver(__i64_run,__i64_i64_call,64)\n"
    "local b=__i64_driver(__u64_run,__i64_u64_call,64)\n"
    "local c=__i64_driver(__i64_vararg_run,__i64_i64_vararg,64)\n"
    "local d=__i64_driver(__u64_vararg_run,__i64_u64_vararg,64)\n"
    "local e=__i64_callm_driver(64)\n"
    "assert(a and b and c and d and e)\n");

  for (i = 0; i < 5; i++) {
    if (i < 4)
      assert(run_loop(L, runners[i], functions[i]));
    else
      assert(run_callm(L));
    init_fixture(L, J, &fx[i], runners[i], i == 4);
    assert_positive(&fx[i], J);
  }

  /* Signedness is paired across the CALLXS result and XSTORE, but the CData
  ** allocation must still match the function's immutable ABI result type. */
  expect_both(&fx[0], J, MUT_CTYPE, 0);
  expect_both(&fx[0], J, MUT_CALL_TYPE, 0);
  expect_both(&fx[0], J, MUT_STORE_TYPE, 0);
  expect_both(&fx[0], J, MUT_PAIRED_UNSIGNED, 0);
  expect_both(&fx[0], J, MUT_CROSS_WIDTH, 0);
  expect_both(&fx[0], J, MUT_OFFSET, 0);
  expect_both(&fx[0], J, MUT_BASE, 0);
  expect_both(&fx[0], J, MUT_STORE_CALL, 0);
  expect_both(&fx[0], J, MUT_ENTRY_ROOT, 0);
  expect_both(&fx[0], J, MUT_REMOVE_STORE, 0);
  expect_both(&fx[0], J, MUT_PHI_INPUT, 0);
  expect_both(&fx[0], J, MUT_PRE_SNAPSHOT_OTHER, 0);
  expect_both(&fx[0], J, MUT_POST_SNAPSHOT_OTHER, 0);
  expect_both(&fx[0], J, MUT_PRE_SNAPSHOT_MISSING, 0);
  expect_both(&fx[0], J, MUT_POST_SNAPSHOT_MISSING, 0);
  expect_both(&fx[0], J, MUT_PRE_SNAPSHOT_NORESTORE, 0);
  expect_both(&fx[0], J, MUT_POST_SNAPSHOT_NORESTORE, 0);
  if (has_spare_call_nop(&fx[0]))
    expect_both(&fx[0], J, MUT_DUP_STORE, 0);
  else
    puts("duplicate XSTORE mutation skipped: trace has no spare in-call NOP");
  expect_both(&fx[1], J, MUT_PAIRED_UNSIGNED, 1);
  expect_both(&fx[1], J, MUT_CTYPE, 1);
  expect_both(&fx[2], J, MUT_VARARG_SEPARATOR, 0);
  expect_both(&fx[3], J, MUT_PAIRED_UNSIGNED, 1);
  expect_both(&fx[3], J, MUT_CROSS_WIDTH, 1);
  expect_both(&fx[4], J, MUT_CTYPE, 0);

  for (i = 0; i < 5; i++) {
    free(fx[i].ir);
    free(fx[i].snap);
    free(fx[i].snapmap);
  }
  lua_close(L);
  puts("t-arm64-jit-callxs-i64-admission OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-i64-admission SKIP: requires experimental Darwin ARM64");
  return 0;
}

#endif
