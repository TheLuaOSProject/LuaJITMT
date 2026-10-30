/* Native pure Lua leaves with integer-overflow exits from a child frame. */
#include <stdio.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/proto_fixture_helpers.h"

typedef struct LeafCase {
  const char *name;
  const char *body;
  BCOp op;
  lua_Integer ranges[2][2];
  unsigned nranges;
  unsigned backedge_mask;
  const char *runner;
} LeafCase;

static unsigned native_runs, child_exits, same_trace_reuses;
static unsigned gc_reuses, cold_retries;
static unsigned semantic_bad, postra_bad, alias_bad;
static unsigned return_certificates;

static void constant_boundary_checks(void)
{
  IRIns *ir = (IRIns *)calloc(REF_FIRST+16u, sizeof(IRIns));
  IRRef nk = REF_TRUE-8u;
  IRRef nins = REF_FIRST+16u;
  IRRef boundary = REF_TRUE-1u;
  assert(ir != NULL && nk < REF_TRUE && nins > REF_FIRST);
  ir[nk].o = IR_KNUM;
  ir[nk+1u].o = IR_KNUM;
  ir[nk+2u].o = IR_KGC;
  ir[nk+3u].o = IR_KGC;
  ir[nk+4u].o = IR_KKPTR;
  ir[nk+5u].o = IR_KKPTR;
  ir[nk+6u].o = IR_KINT;
  ir[nk+6u].i = 0;
  ir[boundary].o = IR_KGC;
  assert(lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk, IR_KNUM));
  assert(!lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk+1u, IR_KNUM));
  assert(lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk+2u, IR_KGC));
  assert(!lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk+3u, IR_KGC));
  assert(lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk+4u, IR_KKPTR));
  assert(!lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk+5u, IR_KKPTR));
  assert(lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk+6u, IR_KINT));
  assert(!lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, nk-1u, IR_KINT));
  assert(!lj_trace_test_arm64_first_side_constant_head(
           ir, nins, nk, boundary, IR_KGC));
  free(ir);
}

static GCfunc *global_function(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return fn;
}

static lua_Number invoke_range(lua_State *L, const char *runner,
                               const char *leaf, lua_Integer first,
                               lua_Integer last)
{
  lua_Number result;
  void *cframe = L->cframe;
  ptrdiff_t base = L->base-tvref(L->stack), top = L->top-tvref(L->stack);
  lua_getglobal(L, runner);
  lua_getglobal(L, leaf);
  lua_pushinteger(L, first);
  lua_pushinteger(L, last);
  ljt_lua_pcall(L, 3, 1, "Lua child exit");
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->base-tvref(L->stack) == base && L->top-tvref(L->stack) == top &&
         L->cframe == cframe && lj_tg_load_jit_base(L2TG(L)) == NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static GCtrace *root_trace(lua_State *L)
{
  GCproto *pt = funcproto(global_function(L, "leaf_run"));
  const BCIns *bc = proto_bc(pt);
  MSize i;
  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_JFORL) {
      TraceNo no = bc_d(ins);
      GCtrace *T = traceref_safe(L2J(L), no);
      if (T && trace_runnable_acq(T, no) && !T->root &&
          trace_startpt_acq(T) == pt && T->linktype == LJ_TRLINK_LOOP &&
          T->link == T->traceno)
        return T;
    }
  }
  return NULL;
}

static const BCIns *only_call(GCproto *pt)
{
  const BCIns *bc = proto_bc(pt), *call = NULL;
  MSize i;
  for (i = 1; i < pt->sizebc; i++) {
    const BCIns *pc = &bc[i];
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)pc)) == BC_CALL) {
      assert(call == NULL);
      call = pc;
    }
  }
  assert(call != NULL);
  return call;
}

static BCReg loop_sum_slot(GCproto *pt, const BCIns *call)
{
  const BCIns *bc = proto_bc(pt);
  BCReg callslot = bc_a((BCIns)la_load32_acq((const uint32_t *)call));
  MSize i;
  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_ADDVV && bc_a(ins) == bc_b(ins) &&
        bc_c(ins) == callslot)
      return bc_a(ins);
  }
  assert(0 && "root accumulator ADDVV missing");
  return 0;
}

static void assert_child_exit(lua_State *L, GCtrace *T,
                              const LeafCase *shape, int backedge)
{
  GCproto *runpt = funcproto(global_function(L, "leaf_run"));
  GCfunc *leaf = global_function(L, "leaf_candidate");
  GCproto *leafpt = funcproto(leaf);
  const BCIns *call = only_call(runpt);
  const BCIns *childpc = proto_bc(leafpt) + 2;
  BCIns add = (BCIns)la_load32_acq((const uint32_t *)childpc);
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo exitno = lj_trace_test_first_exitno();
  const SnapShot *snap;
  const SnapEntry *map;
  MSize ofs, nent, i;
  uint64_t pcbase;
  BCReg frame_slot, arg_slot, temp_slot, sum_slot;
  IRRef arg_ref = 0, temp_ref = 0;
  int found_frame = 0, found_callee = 0, found_sum = 0;

  assert(bc_op(add) == shape->op);
  assert(bc_op((BCIns)la_load32_acq((const uint32_t *)call)) == BC_CALL &&
         bc_b((BCIns)la_load32_acq((const uint32_t *)call)) == 2 &&
         bc_c((BCIns)la_load32_acq((const uint32_t *)call)) == 2);
  assert(parent == T->traceno && exitno < trace_nsnap_acq(T));
  snap = &trace_snap_acq(T)[exitno];
  {
    IRRef loop = 0;
    for (IRRef r = REF_FIRST; r < T->nins; r++)
      if (T->ir[r].o == IR_LOOP) loop = r;
    assert(loop && (backedge ? snap_ref_acq(snap) > loop :
                               snap_ref_acq(snap) < loop));
  }
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  assert(ofs <= trace_nsnapmap_acq(T) &&
         nent + 1u + LJ_FR2 <= trace_nsnapmap_acq(T) - ofs);
  map = &trace_snapmap_acq(T)[ofs];
  memcpy(&pcbase, &map[nent], sizeof(pcbase));
  assert((const BCIns *)(uintptr_t)(pcbase >> 8) == childpc);

  /* The low byte is J->baseslot-2. A root BC_CALL A moves the callee base
  ** by A+1+LJ_FR2 slots from the root base. */
  frame_slot = (BCReg)(1u + LJ_FR2 + bc_a((BCIns)
      la_load32_acq((const uint32_t *)call)) + 1u);
  assert((uint8_t)(pcbase & 0xffu) ==
         (uint8_t)(bc_a((BCIns)la_load32_acq((const uint32_t *)call)) +
                   1u + LJ_FR2));
  arg_slot = frame_slot + 1u;
  temp_slot = arg_slot + 1u;
  sum_slot = (BCReg)(1u + LJ_FR2 + loop_sum_slot(runpt, call));
  assert(snap_nslots_acq(snap) == temp_slot + 1u);

  for (i = 0; i < nent; i++) {
    SnapEntry sn = snapentry_acq(&map[i]);
    IRRef ref = snap_ref(sn);
    const IRIns *ir;
    if (ref >= T->nins) continue;
    ir = &trace_ir_acq(T)[ref];
    if (snap_slot(sn) == frame_slot && (sn & SNAP_FRAME)) {
      assert(ir->o == IR_KNUM && ir_knum(ir)->u64 ==
             (uint64_t)(uintptr_t)(call + 1));
      found_frame = 1;
    }
    if (snap_slot(sn) + 1u == frame_slot && ir->o == IR_KGC) {
      assert(ir_kgc_acq(ir) == obj2gco(leaf));
      found_callee = 1;
    }
    if (snap_slot(sn) == arg_slot) {
      assert(!(sn & (SNAP_NORESTORE | SNAP_FRAME)) &&
             irt_type(ir->t) == IRT_INT);
      arg_ref = ref;
    }
    if (snap_slot(sn) == temp_slot) {
      assert(!(sn & (SNAP_NORESTORE | SNAP_FRAME)) &&
             irt_type(ir->t) == IRT_INT);
      temp_ref = ref;
    }
    if (snap_slot(sn) == sum_slot) {
      assert(!(sn & (SNAP_NORESTORE | SNAP_FRAME)) &&
             irt_type(ir->t) == IRT_NUM);
      found_sum = 1;
    }
  }
  assert(found_frame && found_callee && found_sum && arg_ref != 0 &&
         temp_ref == arg_ref);
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exitno() == exitno);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void native_range(lua_State *L, GCtrace *T, const LeafCase *shape,
                         lua_Integer first, lua_Integer last, int childexit)
{
  lua_Number expected = invoke_range(L, "leaf_oracle_run", "leaf_oracle",
                                    first, last);
  lj_trace_test_arm64_child_return_reset();
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(invoke_range(L, "leaf_run", "leaf_candidate", first, last) ==
         expected);
  if (lj_trace_test_arm64_child_return_count() != 0) {
    LJArm64ChildReturnCert cert;
    assert(lj_trace_test_arm64_child_return_read(&cert));
    assert(cert.valid && cert.nresults == 1 && cert.parentno == T->traceno &&
           cert.parent_generation != 0 && cert.caller_callpc != NULL &&
           cert.caller_returnpc == cert.caller_callpc+1 &&
           cert.childpt != NULL && cert.callerpt == trace_startpt_acq(T) &&
           cert.childfn != NULL && cert.callerfn != NULL &&
           cert.child_startpc != NULL && cert.child_retpc != NULL &&
           cert.caller_result_slot < cert.callerpt->framesize &&
           cert.child_baseslot != 0 && cert.split_nins > REF_FIRST &&
           cert.child_result == cert.caller_result);
    assert(bc_op((BCIns)la_load32_acq(
             (const uint32_t *)cert.caller_callpc)) == BC_CALL &&
           bc_c((BCIns)la_load32_acq(
             (const uint32_t *)cert.caller_callpc)) == 2 &&
           bc_op((BCIns)la_load32_acq(
             (const uint32_t *)cert.child_retpc)) == BC_RET1);
    return_certificates++;
  }
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno);
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  if (childexit) {
    assert_child_exit(L, T, shape, childexit == 2);
    child_exits++;
  } else {
    same_trace_reuses++;
  }
  native_runs++;
}

static void install_case(lua_State *L, const LeafCase *shape)
{
  char source[2048];
  snprintf(source, sizeof(source),
    "local body='%s'; "
    "leaf_candidate=assert(loadstring('return function(x) '..body..' end',"
      "'@leaf-candidate'))(); "
    "leaf_oracle=assert(loadstring('return function(x) '..body..' end',"
      "'@leaf-oracle'))(); "
    "local runner='%s'; "
    "leaf_run=assert(loadstring(runner,'@leaf-run'))(); "
    "leaf_oracle_run=assert(loadstring(runner,'@leaf-oracle-run'))(); "
    "jit.off(leaf_oracle,true); jit.off(leaf_oracle_run,true); "
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000',"
      "'maxtrace=64')",
    shape->body, shape->runner ? shape->runner :
      "return function(fn,first,last) local sum=.5; "
      "for i=first,last do sum=sum+fn(i) end; return sum end");
  ljt_lua_dostring(L, source);
}

static GCtrace *train_case(lua_State *L, const LeafCase *shape)
{
  GCtrace *T = NULL;
  unsigned attempt;
  assert(lj_trace_arm64_lua_leaf(global_function(L, "leaf_candidate")));
  for (attempt = 0; attempt < 8 && !T; attempt++) {
    lua_Number expected = invoke_range(L, "leaf_oracle_run", "leaf_oracle",
                                       1, 96);
    assert(invoke_range(L, "leaf_run", "leaf_candidate", 1, 96) == expected);
    T = root_trace(L);
    if (attempt) cold_retries++;
  }
  (void)shape;
  assert(T != NULL);
  return T;
}

/* Force the interpreter side-exit counter low enough to observe a real
** child-frame side recording attempt. Publication remains subject to the
** production admission gate; this only verifies that record_ret captures a
** source/frame boundary before that later gate runs. */
static void observe_child_return_certificate(lua_State *L)
{
  GCtrace *T;
  LJArm64ChildReturnCert cert;

  lj_trace_test_arm64_child_return_reset();
  ljt_lua_dostring(L,
    "jit.off(true,true); "
      "local source=[=[local f=function(a) if a>20 then return a*.25 end; "
      "if a>10 then return a*.5 end; if a<0 then return a*-1 end; "
      "return a+.25 end; return function(x,n) local sum=.5; "
      "for i=1,n do sum=sum+f(x) end; return sum end]=]; "
      "local function compile(name) return assert(loadstring(source,name))() end; "
      "jit.off(compile,true); leaf_run=compile('@reachable-ssa-candidate'); "
      "leaf_oracle_run=compile('@reachable-ssa-oracle'); "
      "jit.off(leaf_oracle_run,true); "
      "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1','maxtrace=64'); "
      "assert(leaf_run(5,96)==leaf_oracle_run(5,96)); "
      "for _,x in ipairs({30,15,-3,5}) do "
      "assert(leaf_run(x,32)==leaf_oracle_run(x,32)) end");
  T = root_trace(L);
  assert(T != NULL && trace_runnable_acq(T, T->traceno));
  if (lj_trace_test_arm64_child_return_count() == 0)
    fprintf(stderr, "child-return cert absent: aborts=%u last=%d side-gates=%u "
            "root-children=%u captures=%u exit-calls=%u root-entry=%u first=%u/%u\n",
            lj_trace_test_abort_count(),
            (int)lj_trace_test_last_abort_error(),
            lj_trace_test_admission_side_gate_blocks(),
            trace_nchild_acq(T), lj_trace_test_arm64_child_return_count(),
            lj_trace_test_exit_calls(),
            lj_trace_test_root_entry_successes(),
            lj_trace_test_first_exit_parent(), lj_trace_test_first_exitno());
  assert(lj_trace_test_arm64_child_return_count() != 0 &&
         lj_trace_test_arm64_child_return_read(&cert));
  assert(cert.valid && cert.nresults == 1 && cert.parentno == T->traceno &&
         cert.parent_generation != 0 && cert.caller_callpc != NULL &&
         cert.caller_returnpc == cert.caller_callpc+1 &&
         isluafunc(cert.childfn) &&
         cert.childpt == funcproto(cert.childfn) &&
         cert.callerpt == trace_startpt_acq(T) &&
         cert.caller_result_slot < cert.callerpt->framesize &&
         cert.split_nins > REF_FIRST && cert.child_result == cert.caller_result);
  assert(bc_op((BCIns)la_load32_acq(
           (const uint32_t *)cert.caller_callpc)) == BC_CALL &&
         bc_c((BCIns)la_load32_acq(
           (const uint32_t *)cert.caller_callpc)) == 2 &&
         bc_op((BCIns)la_load32_acq(
           (const uint32_t *)cert.child_retpc)) == BC_RET1);
  return_certificates++;
  printf("Lua child return certificate captured from the forced child-return probe; publication remains gated\n");
}

static GCproto *copy_proto(const GCproto *pt)
{
  return ljt_proto_clone_alloc(pt);
}

typedef struct ChildFixture {
  GCtrace *live, copy;
  GCproto *root, *leafpt;
  GCfunc *leaf;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef n, identity, kfunc;
  SnapNo child;
  MSize function_entry, frame_entry, arg_entry, temp_entry;
} ChildFixture;

static void child_reset(ChildFixture *f)
{
  GCtrace *T = f->live;
  GCproto *root = trace_startpt_acq(T);
  GCfunc *leaf = ir_kfunc(&T->ir[f->kfunc]);
  GCproto *pt = funcproto(leaf);
  MSize start = proto_bcpos(root, trace_startpc_acq(T));
  ljt_proto_clone_reset(f->root, root);
  ljt_proto_clone_reset(f->leafpt, pt);
  memset(f->leaf, 0, sizeof(*f->leaf));
  memcpy(f->leaf, leaf, sizeLfunc(0));
  setmref(f->leaf->l.pc, proto_bc(f->leafpt));
  memcpy(f->ir+T->nk, T->ir+T->nk, (T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->kfunc], obj2gco(f->leaf));
  memcpy(f->snap, T->snap, T->nsnap*sizeof(SnapShot));
  memcpy(f->map, T->snapmap, T->nsnapmap*sizeof(SnapEntry));
  f->copy = *T; f->copy.ir = f->ir; f->copy.snap = f->snap;
  f->copy.snapmap = f->map;
  trace_startpt_rel(&f->copy, f->root);
  setmref(f->copy.startpc, proto_bc(f->root)+start);
  proto_bc(f->root)[start] = T->startins;
  for (MSize s = 0; s < T->nsnap; s++) {
    SnapShot *snap = &f->snap[s];
    uint64_t pcbase;
    MSize pos;
    memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], 8);
    if ((uint8_t)pcbase) {
      pos = proto_bcpos(pt, (const BCIns *)(uintptr_t)(pcbase>>8));
      assert(pos < pt->sizebc);
      pcbase = (u64ptr(proto_bc(f->leafpt)+pos)<<8)|(uint8_t)pcbase;
      for (MSize i = 0; i < snap->nent; i++) {
        SnapEntry entry = T->snapmap[snap->mapofs+i];
        if (entry & SNAP_FRAME) {
          IRRef k = snap_ref(entry);
          const BCIns *pc = (const BCIns *)(uintptr_t)T->ir[k+1u].tv.u64;
          MSize retpos = proto_bcpos(root, pc);
          assert(T->ir[k].o == IR_KNUM && retpos < root->sizebc);
          f->ir[k+1u].tv.u64 = u64ptr(proto_bc(f->root)+retpos);
        }
      }
    } else {
      pos = proto_bcpos(root, (const BCIns *)(uintptr_t)(pcbase>>8));
      assert(pos < root->sizebc);
      pcbase = u64ptr(proto_bc(f->root)+pos)<<8;
    }
    memcpy(&f->map[snap->mapofs+snap->nent], &pcbase, 8);
  }
  memset(f->J, 0, sizeof(*f->J));
  f->J->pt = f->root; f->J->startpc = trace_startpc_acq(&f->copy);
  f->J->baseslot = 1+LJ_FR2;
  for (IRRef r = REF_FIRST; r < f->n; r++)
    if (f->ir[r].o == IR_LOOP) f->J->loopref = r;
  f->view = (LJArm64PostRAView){0};
  f->view.ir = f->ir; f->view.owner = &f->copy; f->view.owner_private = 1;
  f->view.snap = f->snap; f->view.snapmap = f->map;
  f->view.proto = f->root; f->view.proto_bc = proto_bc(f->root);
  f->view.startpc = trace_startpc_acq(&f->copy);
  f->view.nins = T->nins; f->view.nk = T->nk;
  f->view.nsnap = T->nsnap; f->view.nsnapmap = T->nsnapmap;
  f->view.spadjust = T->spadjust; f->view.proto_sizebc = f->root->sizebc;
  f->view.proto_numparams = f->root->numparams;
  f->view.root_topslot = T->topslot; f->view.startins = T->startins;
}

static int child_admit(ChildFixture *f, int allocated)
{
  if (allocated) return lj_asm_arm64_postra_admit(&f->view, NULL);
  f->copy.nins = f->n;
  return lj_asm_arm64_ir_admit(f->J, &f->copy, NULL);
}

enum {
  C_BASE_ZERO, C_BASE_SHIFT, C_PC_ROOT, C_PC_RETURN, C_TOP_LOW, C_TOP_HIGH,
  C_SLOTS_LOW, C_SLOTS_HIGH, C_FRAME_FLAG, C_FRAME_CONT, C_FRAME_NORESTORE,
  C_FRAME_VALUE, C_RETURN_LINK, C_FUNCTION_VALUE, C_ARGUMENT, C_TEMP,
  C_BOTH_ARGS, C_NO_GUARD, C_GUARD_CONSTANT, C_CALLEE_ARITY, C_CALLEE_SIZE,
  C_CALLEE_COPY, C_CALLEE_OP, C_CALLEE_OUTPUT, C_CALLEE_RETURN,
  C_CALL_ARITY, C_CALL_A, C_CALL_LINK, C_IDENTITY, C_COUNT
};

static void child_mutate(ChildFixture *f, unsigned m)
{
  SnapShot *snap = &f->snap[f->child];
  IRIns *guard = &f->ir[snap->ref];
  IRRef arg = snap_ref(f->map[f->arg_entry]);
  IRRef frame = snap_ref(f->map[f->frame_entry]);
  BCIns *leafbc = proto_bc(f->leafpt);
  BCIns *call = (BCIns *)(uintptr_t)f->ir[frame+1u].tv.u64-1;
  IRRef k = guard->op1 == arg ? guard->op2 : guard->op1;
  uint64_t pcbase;
  memcpy(&pcbase, &f->map[snap->mapofs+snap->nent], 8);
  switch (m) {
  case C_BASE_ZERO: pcbase &= ~UINT64_C(255); break;
  case C_BASE_SHIFT: pcbase++; break;
  case C_PC_ROOT: pcbase = (u64ptr(call)<<8)|(uint8_t)pcbase; break;
  case C_PC_RETURN: pcbase = (u64ptr(leafbc+3)<<8)|(uint8_t)pcbase; break;
  case C_TOP_LOW: snap->topslot--; break;
  case C_TOP_HIGH: snap->topslot++; break;
  case C_SLOTS_LOW: snap->nslots--; break;
  case C_SLOTS_HIGH: snap->nslots++; break;
  case C_FRAME_FLAG: f->map[f->frame_entry] &= ~SNAP_FRAME; break;
  case C_FRAME_CONT:
    f->map[f->frame_entry] = (f->map[f->frame_entry]&~SNAP_FRAME)|SNAP_CONT; break;
  case C_FRAME_NORESTORE: f->map[f->frame_entry] |= SNAP_NORESTORE; break;
  case C_FRAME_VALUE:
    f->map[f->frame_entry] = SNAP(snap_slot(f->map[f->frame_entry]),SNAP_FRAME,arg); break;
  case C_RETURN_LINK: f->ir[frame+1u].tv.u64 += sizeof(BCIns); break;
  case C_FUNCTION_VALUE:
    f->map[f->function_entry] = SNAP(snap_slot(f->map[f->function_entry]),0,
                                    f->ir[f->identity].op1); break;
  case C_ARGUMENT:
    f->map[f->arg_entry] = SNAP(snap_slot(f->map[f->arg_entry]),0,k); break;
  case C_TEMP:
    f->map[f->temp_entry] = SNAP(snap_slot(f->map[f->temp_entry]),0,k); break;
  case C_BOTH_ARGS:
    f->map[f->arg_entry] = SNAP(snap_slot(f->map[f->arg_entry]),0,k);
    f->map[f->temp_entry] = SNAP(snap_slot(f->map[f->temp_entry]),0,k); break;
  case C_NO_GUARD: guard->t.irt = IRT_INT; break;
  case C_GUARD_CONSTANT: f->ir[k].i ^= 1; break;
  case C_CALLEE_ARITY: f->leafpt->numparams = 2; break;
  case C_CALLEE_SIZE: f->leafpt->sizebc--; break;
  case C_CALLEE_COPY: setbc_d(&leafbc[1],1); break;
  case C_CALLEE_OP:
    setbc_op(&leafbc[2],
      (bc_op(leafbc[2])==BC_MULVN || bc_op(leafbc[2])==BC_MULNV) ?
      BC_ADDVN : BC_MULVN); break;
  case C_CALLEE_OUTPUT: setbc_a(&leafbc[2],0); break;
  case C_CALLEE_RETURN: setbc_a(&leafbc[3],0); break;
  case C_CALL_ARITY: setbc_c(call,3); break;
  case C_CALL_A: setbc_a(call,bc_a(*call)-1u); break;
  case C_CALL_LINK: setbc_op(call,BC_CALLM); break;
  case C_IDENTITY: f->ir[f->identity].t.irt = IRT_FUNC; break;
  default: assert(0);
  }
  memcpy(&f->map[snap->mapofs+snap->nent], &pcbase, 8);
}

static void child_admission(GCtrace *T)
{
  ChildFixture f = {0};
  unsigned children = 0, dynamic_function_aliases = 0;
  f.live = T; f.n = T->nins;
  if (T->ir[f.n-1].o == IR_NOP) f.n--;
  else while (T->ir[f.n-1].o == IR_RENAME) f.n--;
  for (IRRef r = REF_FIRST; r < f.n; r++) {
    if (T->ir[r].o == IR_EQ && T->ir[r].t.irt == (IRT_FUNC|IRT_GUARD)) {
      assert(!f.identity); f.identity = r; f.kfunc = T->ir[r].op2;
    }
  }
  assert(f.identity);
  f.root = copy_proto(trace_startpt_acq(T));
  f.leafpt = copy_proto(funcproto(ir_kfunc(&T->ir[f.kfunc])));
  f.leaf = calloc(1, sizeof(*f.leaf)); f.J = calloc(1, sizeof(*f.J));
  f.ir = calloc(T->nins+1u, sizeof(IRIns)); f.snap = malloc(T->nsnap*sizeof(SnapShot));
  f.map = malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf && f.J && f.ir && f.snap && f.map);
  for (SnapNo s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    uint64_t pcbase;
    MSize base;
    memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], 8);
    if (!(uint8_t)pcbase) continue;
    children++; f.child = s; base = (uint8_t)pcbase+2u;
    f.function_entry = f.frame_entry = f.arg_entry = f.temp_entry = UINT32_MAX;
    for (MSize i = 0; i < snap->nent; i++) {
      MSize at = snap->mapofs+i, slot = snap_slot(T->snapmap[at]);
      if (slot == base-2u) f.function_entry = at;
      if (slot == base-1u) f.frame_entry = at;
      if (slot == base) f.arg_entry = at;
      if (slot == base+1u) f.temp_entry = at;
    }
    assert(f.function_entry != UINT32_MAX && f.frame_entry != UINT32_MAX &&
           f.arg_entry != UINT32_MAX && f.temp_entry != UINT32_MAX);
    for (int allocated = 0; allocated < 2; allocated++) {
      for (unsigned m = 0; m < C_COUNT; m++) {
        /* The identity-guard operand can itself be the exact saved callee
        ** reference. This mutation is therefore not intrinsically invalid;
        ** unrelated frame functions are covered by nested-call mutations. */
        if (m == C_FUNCTION_VALUE) continue;
        child_reset(&f); assert(child_admit(&f, allocated));
        child_reset(&f); child_mutate(&f, m);
        if (child_admit(&f, allocated)) {
          fprintf(stderr, "child mutation=%u snapshot=%u accepted by %s\n",
                  m, s, allocated ? "postRA" : "semantic");
          assert(0);
        }
        if (allocated) postra_bad++; else semantic_bad++;
      }
    }
    /* When the frame slot is exactly the guarded dynamic callee, this is a
    ** valid source form. Require at least one such nontrivial positive in
    ** both independent passes; unrelated dynamic identities are negative in
    ** the nested-call fixture. */
    child_reset(&f); child_mutate(&f, C_FUNCTION_VALUE);
    if (child_admit(&f, 0)) {
      child_reset(&f); child_mutate(&f, C_FUNCTION_VALUE);
      assert(child_admit(&f, 1));
      dynamic_function_aliases++;
    }
    child_reset(&f); assert(child_admit(&f, 1));
    {
      IRRef arg = snap_ref(f.map[f.arg_entry]);
      f.ir[arg].r = RID_D0;
      /* Remove all RENAME overrides for this source to test its real exit
      ** location, rather than a later register name irrelevant to this exit. */
      for (IRRef r = f.n; r < T->nins; r++)
        if (f.ir[r].o == IR_RENAME && f.ir[r].op1 == arg) f.ir[r].r = RID_D0;
      assert(!child_admit(&f, 1)); postra_bad++;
    }
    child_reset(&f); assert(child_admit(&f, 1));
    f.ir[snap_ref(f.map[f.arg_entry])].s = 254;
    assert(!child_admit(&f, 1)); postra_bad++;
    child_reset(&f); assert(child_admit(&f, 1));
    if (snap->ref > f.J->loopref) {
      IRRef a = 0, b = 0;
      RegSP brs = 0;
      for (MSize i = 0; i < snap->nent; i++) {
        SnapEntry entry = f.map[snap->mapofs+i];
        IRRef r = snap_ref(entry);
        if ((entry & 0x00ff0000u) || irref_isk(r) ||
            irt_type(f.ir[r].t) != IRT_NUM) continue;
        RegSP rs = f.ir[r].prev;
        for (IRRef ren = T->nins; ren-- > f.n;) {
          if (f.ir[ren].o == IR_RENAME && f.ir[ren].op1 == r &&
              f.ir[ren].op2 <= s) rs = f.ir[ren].prev;
        }
        if (ra_hasspill(regsp_spill(rs)) || !ra_hasreg(regsp_reg(rs))) continue;
        if (!a) a = r;
        else if (a != r) { b = r; brs = rs; break; }
      }
      if (a && b) {
        IRRef ren = T->nins;
        if (f.ir[ren-1u].o == IR_NOP) ren--;
        f.ir[ren] = (IRIns){0}; f.ir[ren].o = IR_RENAME;
        f.ir[ren].t.irt = IRT_NIL; f.ir[ren].op1 = a; f.ir[ren].op2 = s;
        f.ir[ren].r = regsp_reg(brs); f.ir[ren].s = SPS_NONE;
        f.copy.nins = f.view.nins = ren+1u;
        assert(!child_admit(&f, 1)); postra_bad++; alias_bad++;
      }
    }
  }
  assert(children == 2 && dynamic_function_aliases > 0);
  free(f.root); free(f.leafpt); free(f.leaf); free(f.J); free(f.ir); free(f.snap); free(f.map);
}

int main(void)
{
  static const LeafCase cases[] = {
    {"add", "return x+3", BC_ADDVN,
      {{INT_MAX-10, INT_MAX-1}, {INT_MAX-2, INT_MAX-1}}, 2, 1, NULL},
    {"sub", "return x-3", BC_SUBVN,
      {{INT_MIN+1, INT_MIN+2}}, 1, 0, NULL},
    {"mul", "return x*3", BC_MULVN,
      {{715827879, 715827886}}, 1, 1, NULL},
    {"subnv", "return 3-x", BC_SUBNV,
      {{INT_MIN+1, INT_MIN+2}}, 1, 0, NULL},
    {"addnv", "return 3+x", BC_ADDNV,
      {{INT_MAX-10, INT_MAX-1}, {INT_MAX-2, INT_MAX-1}}, 2, 1, NULL},
    {"mulnv", "return 3*x", BC_MULNV,
      {{715827879, 715827886}}, 1, 1, NULL},
    {"add-pressure", "return x+3", BC_ADDVN,
      {{INT_MAX-10, INT_MAX-1}, {INT_MAX-2, INT_MAX-1}}, 2, 1,
      "return function(fn,first,last) local sum=.5; local carry=.25; "
      "for i=first,last do sum=sum+fn(i)+carry; carry=carry+.25 end; return sum end"}
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  unsigned shape;
  assert(LJ_FR2 == 1);
  constant_boundary_checks();
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "assert(select(2,s:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5)))");
  observe_child_return_certificate(L);
  for (shape = 0; shape < sizeof(cases)/sizeof(cases[0]); shape++) {
    const LeafCase *c = &cases[shape];
    GCtrace *T;
    unsigned r;
    install_case(L, c);
    T = train_case(L, c);
    native_range(L, T, c, 9, 40, 0);
    for (r = 0; r < c->nranges; r++) {
      native_range(L, T, c, c->ranges[r][0], c->ranges[r][1],
                   (c->backedge_mask & (1u << r)) ? 2 : 1);
      native_range(L, T, c, 9, 40, 0);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    native_range(L, T, c, 1, 24, 0);
    gc_reuses++;
    child_admission(T);
    printf("Lua child leaf %s: native trace reused after %u overflow exit(s)\n",
           c->name, c->nranges);
  }
  ljt_lua_dostring(L,
    "assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  assert(alias_bad > 0);
  printf("Lua child exits OK: native=%u child-exits=%u same-trace-reuses=%u "
         "gc-reuses=%u cold-retries=%u semantic-negative=%u postRA-negative=%u "
         "aliases=%u return-certs=%u\n",
         native_runs, child_exits, same_trace_reuses, gc_reuses, cold_retries,
         semantic_bad, postra_bad, alias_bad, return_certificates);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-child-exits SKIP"); return 0; }
#endif
