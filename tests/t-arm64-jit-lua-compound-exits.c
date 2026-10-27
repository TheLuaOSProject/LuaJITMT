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

typedef struct LeafCase {
  const char *name;
  const char *body;
  struct {
    lua_Integer first, last;
    MSize childpc;
    BCOp op;
    int backedge;
  } ranges[8];
  unsigned nranges;
  unsigned nchildops;
  const char *runner;
} LeafCase;

static unsigned native_runs, child_exits, same_trace_reuses;
static unsigned gc_reuses, cold_retries;
static unsigned semantic_bad, postra_bad, alias_bad;

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
                              const LeafCase *shape, MSize childpc_offset,
                              BCOp child_op, int backedge)
{
  GCproto *runpt = funcproto(global_function(L, "leaf_run"));
  GCfunc *leaf = global_function(L, "leaf_candidate");
  GCproto *leafpt = funcproto(leaf);
  const BCIns *call = only_call(runpt);
  const BCIns *childpc = proto_bc(leafpt) + childpc_offset;
  BCIns add = (BCIns)la_load32_acq((const uint32_t *)childpc);
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo exitno = lj_trace_test_first_exitno();
  const SnapShot *snap;
  const SnapEntry *map;
  MSize ofs, nent, i;
  uint64_t pcbase;
  BCReg frame_slot, arg_slot, sum_slot;
  IRRef arg_ref = 0;
  MSize max_child_slot = 0, child_slots = 0;
  int found_frame = 0, found_callee = 0, found_sum = 0;

  assert(bc_op(add) == child_op);
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
  sum_slot = (BCReg)(1u + LJ_FR2 + loop_sum_slot(runpt, call));
  assert(snap_nslots_acq(snap) >= arg_slot + 1u &&
         snap_nslots_acq(snap) <= arg_slot + leafpt->framesize);

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
    if (snap_slot(sn) >= arg_slot) {
      assert(!(sn & (SNAP_NORESTORE | SNAP_FRAME)) &&
             snap_slot(sn) < arg_slot + leafpt->framesize &&
             (irt_type(ir->t) == IRT_INT || irt_type(ir->t) == IRT_NUM));
      child_slots++;
      if (snap_slot(sn) > max_child_slot) max_child_slot = snap_slot(sn);
      if (snap_slot(sn) == arg_slot) arg_ref = ref;
    }
    if (snap_slot(sn) == sum_slot) {
      assert(!(sn & (SNAP_NORESTORE | SNAP_FRAME)) &&
             irt_type(ir->t) == IRT_NUM);
      found_sum = 1;
    }
  }
  assert(found_frame && found_callee && found_sum && arg_ref != 0 &&
         max_child_slot + 1u == snap_nslots_acq(snap) &&
         child_slots == max_child_slot - arg_slot + 1u);
  assert(shape->nchildops >= 2);
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exitno() == exitno);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void native_range(lua_State *L, GCtrace *T, const LeafCase *shape,
                         lua_Integer first, lua_Integer last,
                         MSize childpc_offset, BCOp child_op,
                         int childexit)
{
  lua_Number expected = invoke_range(L, "leaf_oracle_run", "leaf_oracle",
                                    first, last);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(invoke_range(L, "leaf_run", "leaf_candidate", first, last) ==
         expected);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno);
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  if (childexit) {
    assert_child_exit(L, T, shape, childpc_offset, child_op, childexit == 2);
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

static GCproto *copy_proto(const GCproto *pt)
{
  size_t bytes = pt->sizept;
  assert(bytes >= sizeof(GCproto)+pt->sizebc*sizeof(BCIns));
  GCproto *copy = malloc(bytes);
  assert(copy); memcpy(copy, pt, bytes); return copy;
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
  memcpy(f->root, root, root->sizept);
  memcpy(f->leafpt, pt, pt->sizept);
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

static RegSP child_location(ChildFixture *f, IRRef ref, SnapNo snap)
{
  RegSP rs = f->ir[ref].prev;
  for (IRRef ren = f->live->nins; ren-- > f->n;)
    if (f->ir[ren].o == IR_RENAME && f->ir[ren].op1 == ref &&
        f->ir[ren].op2 <= snap)
      rs = f->ir[ren].prev;
  return rs;
}

enum {
  C_BASE_ZERO, C_BASE_SHIFT, C_PC_ROOT, C_PC_RETURN, C_TOP_LOW, C_TOP_HIGH,
  C_SLOTS_LOW, C_SLOTS_HIGH, C_FRAME_FLAG, C_FRAME_CONT, C_FRAME_NORESTORE,
  C_FRAME_VALUE, C_RETURN_LINK, C_FUNCTION_VALUE, C_ARGUMENT,
  C_NO_GUARD, C_GUARD_CONSTANT, C_CALLEE_ARITY, C_CALLEE_SIZE,
  C_CALLEE_COPY, C_CALLEE_OP, C_CALLEE_OUTPUT, C_CALLEE_RETURN,
  C_CALL_ARITY, C_CALL_A, C_CALL_LINK, C_IDENTITY,
  C_NORESTORE_LOCAL, C_LOCAL_REF, C_CHILD_PC, C_CHILD_PREFIX, C_OMIT_ENTRY, C_COUNT
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
  MSize childpc;
  MSize local_entry = UINT32_MAX;
  uint64_t pcbase;
  memcpy(&pcbase, &f->map[snap->mapofs+snap->nent], 8);
  childpc = proto_bcpos(f->leafpt,
      (const BCIns *)(uintptr_t)(pcbase >> 8));
  assert(childpc < f->leafpt->sizebc);
  for (MSize i = 0; i < snap->nent; i++) {
    MSize at = snap->mapofs+i;
    if (snap_slot(f->map[at]) > snap_slot(f->map[f->arg_entry])) {
      local_entry = at; break;
    }
  }
  switch (m) {
  case C_BASE_ZERO: pcbase &= ~UINT64_C(255); break;
  case C_BASE_SHIFT: pcbase++; break;
  case C_PC_ROOT: pcbase = (u64ptr(call)<<8)|(uint8_t)pcbase; break;
  case C_PC_RETURN:
    pcbase = (u64ptr(leafbc+f->leafpt->sizebc-1u)<<8)|(uint8_t)pcbase;
    break;
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
  case C_NO_GUARD: guard->t.irt = IRT_INT; break;
  case C_GUARD_CONSTANT: f->ir[k].i ^= 1; break;
  case C_CALLEE_ARITY: f->leafpt->numparams = 2; break;
  case C_CALLEE_SIZE: f->leafpt->sizebc--; break;
  case C_CALLEE_COPY: setbc_d(&leafbc[1],1); break;
  case C_CALLEE_OP: {
    BCOp op = bc_op(leafbc[childpc]);
    BCOp replacement = op;
    if (op == BC_ADDVN) replacement = BC_SUBVN;
    else if (op == BC_SUBVN) replacement = BC_ADDVN;
    else if (op == BC_MULVN) replacement = BC_ADDVN;
    else if (op == BC_ADDVV) replacement = BC_SUBVV;
    else if (op == BC_SUBVV) replacement = BC_ADDVV;
    else if (op == BC_MULVV) replacement = BC_ADDVV;
    assert(replacement != op);
    setbc_op(&leafbc[childpc], replacement);
    break;
  }
  case C_CALLEE_OUTPUT: {
    BCReg a = bc_a(leafbc[childpc]);
    setbc_a(&leafbc[childpc], (BCReg)((a+1u) % f->leafpt->framesize));
    break;
  }
  case C_CALLEE_RETURN: {
    MSize pc;
    for (pc = 1; pc < f->leafpt->sizebc; pc++)
      if (bc_op(leafbc[pc]) == BC_RET1) break;
    assert(pc < f->leafpt->sizebc);
    setbc_a(&leafbc[pc],
      (BCReg)((bc_a(leafbc[pc])+1u) % f->leafpt->framesize));
    break;
  }
  case C_CALL_ARITY: setbc_c(call,3); break;
  case C_CALL_A: setbc_a(call,bc_a(*call)-1u); break;
  case C_CALL_LINK: setbc_op(call,BC_CALLM); break;
  case C_IDENTITY: f->ir[f->identity].t.irt = IRT_FUNC; break;
  case C_NORESTORE_LOCAL:
    assert(local_entry != UINT32_MAX);
    f->map[local_entry] |= SNAP_NORESTORE; break;
  case C_LOCAL_REF:
    assert(local_entry != UINT32_MAX);
    assert(irt_type(f->ir[k].t) ==
           irt_type(f->ir[snap_ref(f->map[local_entry])].t));
    f->map[local_entry] = SNAP(snap_slot(f->map[local_entry]),0,k); break;
  case C_OMIT_ENTRY:
    assert(local_entry != UINT32_MAX);
    memmove(&f->map[local_entry], &f->map[local_entry+1u],
            (f->copy.nsnapmap-local_entry-1u)*sizeof(SnapEntry));
    snap->nent--;
    for (MSize s = f->child+1u; s < f->copy.nsnap; s++) f->snap[s].mapofs--;
    f->copy.nsnapmap--; f->view.nsnapmap--;
    break;
  case C_CHILD_PC:
    pcbase = (u64ptr(leafbc+1u)<<8)|(uint8_t)pcbase;
    break;
  case C_CHILD_PREFIX: {
    MSize pc;
    for (pc = 1; pc < childpc; pc++)
      if (bc_op(leafbc[pc]) == BC_CGET) break;
    assert(pc < childpc && bc_d(leafbc[pc]) + 1u < f->leafpt->framesize);
    setbc_d(&leafbc[pc], bc_d(leafbc[pc])+1u);
    break;
  }
  default: assert(0);
  }
  memcpy(&f->map[snap->mapofs+snap->nent], &pcbase, 8);
}

static void child_admission(GCtrace *T, const LeafCase *shape)
{
  ChildFixture f = {0};
  GCproto *live_leafpt;
  unsigned children = 0;
  f.live = T; f.n = T->nins;
  if (T->ir[f.n-1].o == IR_NOP) f.n--;
  else while (T->ir[f.n-1].o == IR_RENAME) f.n--;
  for (IRRef r = REF_FIRST; r < f.n; r++) {
    if (T->ir[r].o == IR_EQ && T->ir[r].t.irt == (IRT_FUNC|IRT_GUARD)) {
      assert(!f.identity); f.identity = r; f.kfunc = T->ir[r].op2;
    }
  }
  assert(f.identity);
  live_leafpt = funcproto(ir_kfunc(&T->ir[f.kfunc]));
  f.root = copy_proto(trace_startpt_acq(T));
  f.leafpt = copy_proto(live_leafpt);
  f.leaf = calloc(1, sizeof(*f.leaf)); f.J = calloc(1, sizeof(*f.J));
  f.ir = calloc(T->nins+1u, sizeof(IRIns)); f.snap = malloc(T->nsnap*sizeof(SnapShot));
  f.map = malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf && f.J && f.ir && f.snap && f.map);
  for (SnapNo s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    uint64_t pcbase;
    MSize base, local_entry = UINT32_MAX;
    memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], 8);
    if (!(uint8_t)pcbase) continue;
    {
      const BCIns *childpc = (const BCIns *)(uintptr_t)(pcbase >> 8);
      if (childpc < proto_bc(live_leafpt) ||
          childpc >= proto_bc(live_leafpt)+live_leafpt->sizebc) continue;
    }
    children++; f.child = s; base = (uint8_t)pcbase+2u;
    f.function_entry = f.frame_entry = f.arg_entry = f.temp_entry = UINT32_MAX;
    for (MSize i = 0; i < snap->nent; i++) {
      MSize at = snap->mapofs+i, slot = snap_slot(T->snapmap[at]);
      if (slot == base-2u) f.function_entry = at;
      if (slot == base-1u) f.frame_entry = at;
      if (slot == base) f.arg_entry = at;
      if (slot > base && slot < base+f.leafpt->framesize &&
          local_entry == UINT32_MAX) local_entry = at;
    }
    assert(f.function_entry != UINT32_MAX && f.frame_entry != UINT32_MAX &&
           f.arg_entry != UINT32_MAX);
    f.temp_entry = local_entry;
    for (int allocated = 0; allocated < 2; allocated++) {
      for (unsigned m = 0; m < C_COUNT; m++) {
        IRIns *g;
        IRRef arg, alt;
        int applicable = 1;
        child_reset(&f);
        if (!child_admit(&f, allocated)) {
          fprintf(stderr, "compound original snapshot=%u rejected by %s\n",
                  s, allocated ? "postRA" : "semantic");
          assert(0);
        }
        g = &f.ir[f.snap[f.child].ref];
        arg = snap_ref(f.map[f.arg_entry]);
        alt = g->op1 == arg ? g->op2 : g->op1;
        if (m == C_ARGUMENT &&
            (alt == arg || irt_type(f.ir[alt].t) != irt_type(f.ir[arg].t)))
          applicable = 0;
        if ((m == C_NORESTORE_LOCAL || m == C_LOCAL_REF) &&
            local_entry == UINT32_MAX) applicable = 0;
        if (m == C_LOCAL_REF && local_entry != UINT32_MAX &&
            (alt == snap_ref(f.map[local_entry]) ||
             irt_type(f.ir[alt].t) !=
             irt_type(f.ir[snap_ref(f.map[local_entry])].t))) applicable = 0;
        if (m == C_GUARD_CONSTANT && !irref_isk(alt)) applicable = 0;
        if (m == C_CHILD_PREFIX) {
          const BCIns *childpc = (const BCIns *)(uintptr_t)(pcbase >> 8);
          MSize p, end = proto_bcpos(live_leafpt, childpc);
          for (p = 1; p < end; p++)
            if (bc_op(proto_bc(f.leafpt)[p]) == BC_CGET) break;
          if (p >= end ||
              bc_d(proto_bc(f.leafpt)[p])+1u >= f.leafpt->framesize)
            applicable = 0;
        }
        if (m == C_CALLEE_OP) {
          const BCIns *childpc = (const BCIns *)(uintptr_t)(pcbase >> 8);
          BCOp op = bc_op((BCIns)la_load32_acq((const uint32_t *)childpc));
          if (op != BC_ADDVN && op != BC_SUBVN && op != BC_MULVN &&
              op != BC_ADDVV && op != BC_SUBVV && op != BC_MULVV)
            applicable = 0;
        }
        if (!applicable) continue;
        child_reset(&f); child_mutate(&f, m);
        if (child_admit(&f, allocated)) {
          fprintf(stderr, "child mutation=%u snapshot=%u accepted by %s\n",
                  m, s, allocated ? "postRA" : "semantic");
          assert(0);
        }
        if (allocated) postra_bad++; else semantic_bad++;
      }
    }
    child_reset(&f); assert(child_admit(&f, 1));
    {
      IRRef a = 0, b = 0;
      RegSP alias_reg = 0, a_loc = 0;
      for (MSize i = 0; i < snap->nent; i++) {
        SnapEntry e = f.map[snap->mapofs+i];
        IRRef r = snap_ref(e);
        RegSP rs;
        if (snap_slot(e) < base || irref_isk(r) || r >= f.n) continue;
        rs = child_location(&f, r, s);
        if ((e & 0x00ff0000u) || ra_hasspill(regsp_spill(rs)) ||
            !ra_hasreg(regsp_reg(rs))) continue;
        if (!a) { a = r; a_loc = rs; }
        else if (r != a && irt_type(f.ir[r].t) == irt_type(f.ir[a].t) &&
                 rs != a_loc) {
          b = r; alias_reg = regsp_reg(rs); break;
        }
      }
      if (a && b) {
        f.ir[a].r = alias_reg; f.ir[a].s = SPS_NONE;
        for (IRRef r = f.n; r < T->nins; r++)
          if (f.ir[r].o == IR_RENAME && f.ir[r].op1 == a &&
              f.ir[r].op2 <= s) {
            f.ir[r].r = alias_reg; f.ir[r].s = SPS_NONE;
          }
        assert(!child_admit(&f, 1)); postra_bad++; alias_bad++;
      }
    }
    if (local_entry != UINT32_MAX) {
      IRRef r;
      child_reset(&f); assert(child_admit(&f, 1));
      r = snap_ref(f.map[local_entry]);
      if (!irref_isk(r)) {
        f.ir[r].s = 254;
        assert(!child_admit(&f, 1)); postra_bad++;
      }
    }
  }
  {
    unsigned arith = 0;
    const BCIns *bc;
    child_reset(&f);
    bc = proto_bc(f.leafpt);
    for (MSize pc = 1; pc < f.leafpt->sizebc; pc++) {
      BCOp op = bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[pc]));
      if (op == BC_ADDVN || op == BC_SUBVN || op == BC_MULVN ||
          op == BC_ADDNV || op == BC_SUBNV || op == BC_MULNV ||
          op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV)
        arith++;
    }
    assert(arith == shape->nchildops && children >= 2);
  }
  free(f.root); free(f.leafpt); free(f.leaf); free(f.J); free(f.ir); free(f.snap); free(f.map);
}

int main(void)
{
  static const LeafCase cases[] = {
    {"chain-two", "return (x+3)*2",
      {{INT_MAX-3, INT_MAX-2, 2, BC_ADDVN, 0},
       {1073741820, 1073741821, 2, BC_ADDVN, 0},
       {1073741819, 1073741821, 2, BC_ADDVN, 1}}, 3, 2, NULL},
    {"chain-locals", "local a=x+3; local b=a*3; local c=x-5; return b+c",
      {{INT_MAX-3, INT_MAX-2, 2, BC_ADDVN, 0},
       {715827879, 715827880, 4, BC_MULVN, 0},
       {536870910, 536870911, 9, BC_ADDVV, 0},
       {536870909, 536870911, 9, BC_ADDVV, 1}}, 4, 4, NULL},
    {"cse", "return (x+3)+(x+3)",
      {{INT_MAX-3, INT_MAX-2, 2, BC_ADDVN, 0},
       {1073741820, 1073741821, 4, BC_ADDVN, 0},
       {1073741819, 1073741821, 4, BC_ADDVN, 1}}, 3, 3, NULL},
    {"square", "return x*x+3",
      {{46340, 46341, 3, BC_MULVV, 0}}, 1, 2, NULL},
    {"num-tail", "return (x+3)*2.5",
      {{INT_MAX-3, INT_MAX-2, 2, BC_ADDVN, 0}}, 1, 2, NULL},
    {"kshort-local", "local a=3; return (x+a)*4",
      {{INT_MAX-3, INT_MAX-2, 4, BC_ADDVV, 0},
       {536870908, 536870909, 4, BC_ADDVV, 0}}, 2, 2, NULL},
    {"nvchain", "return 100-(x+3)*4",
      {{INT_MAX-3, INT_MAX-2, 2, BC_ADDVN, 0},
       {536870908, 536870909, 2, BC_ADDVN, 0},
       {-536870891, -536870890, 2, BC_ADDVN, 0}}, 3, 3, NULL}
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  unsigned shape;
  assert(LJ_FR2 == 1);
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "assert(select(2,s:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5)))");
  for (shape = 0; shape < sizeof(cases)/sizeof(cases[0]); shape++) {
    const LeafCase *c = &cases[shape];
    GCtrace *T;
    unsigned r;
    install_case(L, c);
    T = train_case(L, c);
    native_range(L, T, c, 9, 40, 0, BC_ADDVN, 0);
    for (r = 0; r < c->nranges; r++) {
      native_range(L, T, c, c->ranges[r].first, c->ranges[r].last,
                   c->ranges[r].childpc, c->ranges[r].op,
                   c->ranges[r].backedge ? 2 : 1);
      native_range(L, T, c, 9, 40, 0, BC_ADDVN, 0);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    native_range(L, T, c, 1, 24, 0, BC_ADDVN, 0);
    gc_reuses++;
    child_admission(T, c);
    printf("Lua compound child %s: native trace reused after %u overflow exit(s)\n",
           c->name, c->nranges);
  }
  ljt_lua_dostring(L,
    "assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  assert(native_runs == 48 && child_exits == 17 &&
         same_trace_reuses == 31 && gc_reuses == 7);
  assert(alias_bad > 0);
  printf("Lua compound child exits OK: native=%u child-exits=%u same-trace-reuses=%u "
         "gc-reuses=%u cold-retries=%u semantic-negative=%u postRA-negative=%u aliases=%u\n",
         native_runs, child_exits, same_trace_reuses, gc_reuses, cold_retries,
         semantic_bad, postra_bad, alias_bad);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-compound-exits SKIP"); return 0; }
#endif
