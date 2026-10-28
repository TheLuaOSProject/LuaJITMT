/* Fixed-arity pure Lua calls with caller argument snapshots across child exits. */
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

typedef struct ArgCase {
  const char *name, *body, *runner;
  BCReg nargs;
  MSize exitpc;
  struct { lua_Integer first, last; int backedge; } ranges[2];
  unsigned nranges;
} ArgCase;

typedef struct ArgFixture {
  GCtrace *live, copy;
  GCproto *root, *leafpt;
  GCfunc *leaf;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef n, identity, kfunc, loopref;
  SnapNo child;
  SnapNo caller;
  BCReg argbase;
  MSize argentry[8], callerentry[8];
} ArgFixture;

static unsigned native_runs, child_exits, same_trace_reuses;
static unsigned gc_reuses, cold_retries;
static unsigned semantic_bad, postra_bad, alias_bad;
static unsigned mutation_semantic[32], mutation_postra[32];

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
  ljt_lua_pcall(L, 3, 1, "Lua fixed-arity call");
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
  for (MSize pc = 1; pc < pt->sizebc; pc++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pc]);
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
  const BCIns *call = NULL, *bc = proto_bc(pt);
  for (MSize pc = 1; pc < pt->sizebc; pc++) {
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[pc])) == BC_CALL) {
      assert(call == NULL);
      call = &bc[pc];
    }
  }
  assert(call != NULL);
  return call;
}

static void assert_child_exit(lua_State *L, GCtrace *T,
                              const ArgCase *shape, int backedge)
{
  GCproto *runpt = funcproto(global_function(L, "leaf_run"));
  GCfunc *leaf = global_function(L, "leaf_candidate");
  GCproto *leafpt = funcproto(leaf);
  const BCIns *call = only_call(runpt);
  const BCIns *childpc = proto_bc(leafpt) + shape->exitpc;
  ExitNo exitno = lj_trace_test_first_exitno();
  const SnapShot *snap;
  const SnapEntry *map;
  IRRef loop = 0;
  MSize ofs, nent;
  uint64_t pcbase;
  BCReg frame_slot, arg_slot;
  int found_frame = 0, found_func = 0;

  assert(bc_op((BCIns)la_load32_acq((const uint32_t *)call)) == BC_CALL &&
         bc_b((BCIns)la_load32_acq((const uint32_t *)call)) == 2 &&
         bc_c((BCIns)la_load32_acq((const uint32_t *)call)) == shape->nargs+1u &&
         leafpt->numparams == shape->nargs);
  for (IRRef r = REF_FIRST; r < T->nins; r++)
    if (T->ir[r].o == IR_LOOP) loop = r;
  assert(loop && exitno < trace_nsnap_acq(T));
  snap = &trace_snap_acq(T)[exitno];
  assert(backedge ? snap_ref_acq(snap) > loop : snap_ref_acq(snap) < loop);
  ofs = snap_mapofs_acq(snap); nent = snap_nent_acq(snap);
  assert(ofs <= trace_nsnapmap_acq(T) &&
         nent + 1u + LJ_FR2 <= trace_nsnapmap_acq(T)-ofs);
  map = &trace_snapmap_acq(T)[ofs];
  memcpy(&pcbase, &map[nent], sizeof(pcbase));
  assert((const BCIns *)(uintptr_t)(pcbase >> 8) == childpc);

  frame_slot = (BCReg)(1u + LJ_FR2 + bc_a((BCIns)
      la_load32_acq((const uint32_t *)call)) + 1u);
  assert((uint8_t)pcbase == (uint8_t)(bc_a((BCIns)
      la_load32_acq((const uint32_t *)call)) + 1u + LJ_FR2));
  arg_slot = frame_slot + 1u;
  for (MSize i = 0; i < nent; i++) {
    SnapEntry entry = snapentry_acq(&map[i]);
    IRRef ref = snap_ref(entry);
    const IRIns *ir = ref < T->nins ? &trace_ir_acq(T)[ref] : NULL;
    if (!ir) continue;
    if (snap_slot(entry) == frame_slot && (entry & SNAP_FRAME)) {
      assert(ir->o == IR_KNUM && ir_knum(ir)->u64 ==
             (uint64_t)(uintptr_t)(call+1));
      found_frame = 1;
    }
    if (snap_slot(entry)+1u == frame_slot && ir->o == IR_KGC &&
        ir_kgc_acq(ir) == obj2gco(leaf)) found_func = 1;
    if (snap_slot(entry) >= arg_slot &&
        snap_slot(entry) < arg_slot+leafpt->framesize) {
      assert(!(entry & (SNAP_NORESTORE|SNAP_FRAME)) && (irt_type(ir->t) == IRT_INT || irt_type(ir->t) == IRT_NUM));
    }
  }
  assert(found_frame && found_func);
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exitno() == exitno);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void native_range(lua_State *L, GCtrace *T, const ArgCase *shape,
                         lua_Integer first, lua_Integer last, int childexit)
{
  lua_Number expected = invoke_range(L, "leaf_oracle_run", "leaf_oracle",
                                    first, last);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(invoke_range(L, "leaf_run", "leaf_candidate", first, last) == expected);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  if (!lj_trace_test_root_entry_successes() || !lj_trace_test_exit_calls())
    fprintf(stderr, "argument native failure %s [%lld,%lld] entries=%u exits=%u\n",
            shape->name, (long long)first, (long long)last,
            lj_trace_test_root_entry_successes(), lj_trace_test_exit_calls());
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

static void install_case(lua_State *L, const ArgCase *shape)
{
  char source[4096];
  const char *params = shape->nargs == 0 ? "" :
                       shape->nargs == 2 ? "a,b" :
                       shape->nargs == 3 ? "a,b,c" :
                       shape->nargs == 4 ? "a,b,c,d" : "a,b,c,d,e,f,g,h";
  snprintf(source, sizeof(source),
    "leaf_candidate=assert(loadstring('return function(%s) %s end',"
      "'@args-candidate-%s'))(); "
    "leaf_oracle=assert(loadstring('return function(%s) %s end',"
      "'@args-oracle-%s'))(); "
    "leaf_run=assert(loadstring('%s','@args-run-%s'))(); "
    "leaf_oracle_run=assert(loadstring('%s','@args-oracle-run-%s'))(); "
    "jit.off(leaf_oracle,true); jit.off(leaf_oracle_run,true); "
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000',"
      "'maxtrace=64')",
    params, shape->body, shape->name, params, shape->body, shape->name,
    shape->runner, shape->name, shape->runner, shape->name);
  ljt_lua_dostring(L, source);
}

static GCtrace *train_case(lua_State *L)
{
  GCtrace *T = NULL;
  assert(lj_trace_arm64_lua_leaf(global_function(L, "leaf_candidate")));
  for (unsigned i = 0; i < 8 && !T; i++) {
    lua_Number oracle = invoke_range(L, "leaf_oracle_run", "leaf_oracle", 1, 96);
    assert(invoke_range(L, "leaf_run", "leaf_candidate", 1, 96) == oracle);
    T = root_trace(L);
    if (i) cold_retries++;
  }
  assert(T != NULL);
  return T;
}

static GCproto *copy_proto(const GCproto *pt)
{
  size_t bytes = pt->sizept;
  assert(bytes >= sizeof(GCproto)+pt->sizebc*sizeof(BCIns));
  GCproto *copy = malloc(bytes);
  assert(copy != NULL);
  memcpy(copy, pt, bytes);
  return copy;
}

static void reset_fixture(ArgFixture *f)
{
  GCtrace *T = f->live;
  GCproto *root = trace_startpt_acq(T);
  GCfunc *leaf = ir_kfunc(&T->ir[f->kfunc]);
  GCproto *pt = funcproto(leaf);
  MSize start = proto_bcpos(root, trace_startpc_acq(T));
  memcpy(f->root, root, root->sizept);
  memcpy(f->leafpt, pt, pt->sizept);
  setmref(f->root->jit_startins, proto_bc(f->root)+f->root->sizebc);
  setmref(f->leafpt->jit_startins, proto_bc(f->leafpt)+f->leafpt->sizebc);
  memset(f->leaf, 0, sizeof(*f->leaf));
  memcpy(f->leaf, leaf, sizeLfunc(0));
  setmref(f->leaf->l.pc, proto_bc(f->leafpt));
  memcpy(f->ir+T->nk, T->ir+T->nk, (T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->kfunc], obj2gco(f->leaf));
  memcpy(f->snap, T->snap, T->nsnap*sizeof(SnapShot));
  memcpy(f->map, T->snapmap, T->nsnapmap*sizeof(SnapEntry));
  f->copy = *T;
  f->copy.ir = f->ir; f->copy.snap = f->snap; f->copy.snapmap = f->map;
  trace_startpt_rel(&f->copy, f->root);
  setmref(f->copy.startpc, proto_bc(f->root)+start);
  proto_bc(f->root)[start] = T->startins;
  for (SnapNo s = 0; s < T->nsnap; s++) {
    SnapShot *snap = &f->snap[s];
    uint64_t pcbase;
    MSize pos;
    memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
    if ((uint8_t)pcbase) {
      pos = proto_bcpos(pt, (const BCIns *)(uintptr_t)(pcbase >> 8));
      assert(pos < pt->sizebc);
      pcbase = (u64ptr(proto_bc(f->leafpt)+pos)<<8)|(uint8_t)pcbase;
      for (MSize i = 0; i < snap->nent; i++) {
        SnapEntry e = T->snapmap[snap->mapofs+i];
        if (e & SNAP_FRAME) {
          IRRef k = snap_ref(e);
          const BCIns *retpc = (const BCIns *)(uintptr_t)T->ir[k+1u].tv.u64;
          MSize retpos = proto_bcpos(root, retpc);
          assert(T->ir[k].o == IR_KNUM && retpos < root->sizebc);
          f->ir[k+1u].tv.u64 = u64ptr(proto_bc(f->root)+retpos);
        }
      }
    } else {
      pos = proto_bcpos(root, (const BCIns *)(uintptr_t)(pcbase >> 8));
      assert(pos < root->sizebc);
      pcbase = u64ptr(proto_bc(f->root)+pos)<<8;
    }
    memcpy(&f->map[snap->mapofs+snap->nent], &pcbase, sizeof(pcbase));
  }
  memset(f->J, 0, sizeof(*f->J));
  f->J->pt = f->root; f->J->startpc = trace_startpc_acq(&f->copy);
  f->J->baseslot = 1+LJ_FR2;
  for (IRRef r = REF_FIRST; r < f->n; r++)
    if (f->ir[r].o == IR_LOOP) {
      f->loopref = r;
      f->J->loopref = r;
    }
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

static int admit(ArgFixture *f, int allocated)
{
  if (allocated) return lj_asm_arm64_postra_admit(&f->view, NULL);
  f->copy.nins = f->n;
  return lj_asm_arm64_ir_admit(f->J, &f->copy, NULL);
}

static RegSP argument_location(const ArgFixture *f, IRRef ref, SnapNo s)
{
  RegSP rs = f->ir[ref].prev;
  for (IRRef r = f->live->nins; r-- > f->n;)
    if (f->ir[r].o == IR_RENAME && f->ir[r].op1 == ref && f->ir[r].op2 <= s)
      rs = f->ir[r].prev;
  return rs;
}

static const BCIns *root_call(GCproto *pt, BCReg nargs)
{
  const BCIns *bc = proto_bc(pt), *found = NULL;
  for (MSize pc = 1; pc < pt->sizebc; pc++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pc]);
    if (bc_op(ins) == BC_CALL) {
      assert(found == NULL);
      found = &bc[pc];
    }
  }
  assert(found && bc_b(*found) == 2 && bc_c(*found) == nargs+1u);
  return found;
}

static MSize find_map_entry(const SnapEntry *map, MSize ofs, MSize nent,
                            MSize slot)
{
  for (MSize i = 0; i < nent; i++)
    if (snap_slot(map[ofs+i]) == slot) return ofs+i;
  return UINT32_MAX;
}

static uint64_t snapshot_footer(const GCtrace *T, SnapNo s)
{
  const SnapShot *snap = &T->snap[s];
  uint64_t footer;
  memcpy(&footer, &T->snapmap[snap->mapofs+snap->nent], sizeof(footer));
  return footer;
}

static void remove_map_entry(ArgFixture *f, SnapNo target, MSize entry)
{
  SnapShot *snap = &f->snap[target];
  memmove(&f->map[entry], &f->map[entry+1u],
          (f->copy.nsnapmap-entry-1u)*sizeof(SnapEntry));
  snap->nent--;
  for (SnapNo s = target+1u; s < f->copy.nsnap; s++) f->snap[s].mapofs--;
  f->copy.nsnapmap--; f->view.nsnapmap--;
}

enum {
  M_CALL_A, M_CALL_B, M_CALL_C, M_NUMPARAMS,
  M_HEADER_A, M_HEADER_ZERO, M_HEADER_OP, M_HEADER_SHADOW, M_HEADER_SHADOW_PTR,
  M_INDEX_MARK, M_INDEX_MODE, M_ALIAS_PHI_TYPE, M_ALIAS_PHI_SOURCE,
  M_CHILD_SWAP, M_CHILD_NORESTORE, M_CHILD_MISSING,
  M_CHILD_WRONG_PARTITION, M_CHILD_STALE,
  M_CALLER_SWAP, M_CALLER_NORESTORE, M_CALLER_MISSING,
  M_CALLER_WRONG_PARTITION, M_CALLER_STALE, M_COUNT
};

static int mutate(ArgFixture *f, unsigned mutation, BCReg nargs,
                  const IRRef *other_partition, const IRRef *stale_argument)
{
  SnapShot *snap = &f->snap[f->child];
  const BCIns *orig_call = root_call(f->root, nargs);
  BCIns *call = (BCIns *)(uintptr_t)orig_call;
  MSize entry0 = f->argentry[0], entry1 = f->argentry[1];
  switch (mutation) {
  case M_CALL_A:
    if (bc_a(*call) == 0) return 0;
    setbc_a(call, bc_a(*call)-1u); break;
  case M_CALL_B:
    setbc_b(call, 1); break;
  case M_CALL_C:
    setbc_c(call, nargs); break;
  case M_NUMPARAMS:
    if (f->leafpt->numparams != nargs || nargs < 2) return 0;
    f->leafpt->numparams--; break;
  case M_HEADER_A: case M_HEADER_ZERO: case M_HEADER_OP:
  case M_HEADER_SHADOW: case M_HEADER_SHADOW_PTR: {
    BCIns *header = proto_bc(f->leafpt);
    if (bc_op(*header) != BC_JFUNCF) return 0;
    if (mutation == M_HEADER_A) setbc_a(header, bc_a(*header)+1u);
    else if (mutation == M_HEADER_ZERO) setbc_d(header, 0);
    else if (mutation == M_HEADER_OP) setbc_op(header, BC_IFUNCF);
    else if (mutation == M_HEADER_SHADOW) proto_jit_startins(f->leafpt)[0] = 0;
    else setmref(f->leafpt->jit_startins, NULL);
    break;
  }
  case M_INDEX_MARK: case M_INDEX_MODE:
  case M_ALIAS_PHI_TYPE: case M_ALIAS_PHI_SOURCE: {
    MSize indexslot = 1u+LJ_FR2+bc_a(f->copy.startins);
    IRRef index = 0, alias = 0;
    for (IRRef r = REF_FIRST; r < f->loopref; r++)
      if (f->ir[r].o == IR_SLOAD && f->ir[r].op1 == indexslot) index = r;
    if (!index || !irt_isphi(f->ir[index].t)) return 0;
    for (IRRef r = f->loopref+1u; r < f->n; r++)
      if (f->ir[r].o == IR_PHI && f->ir[r].op1 == index) alias = r;
    assert(alias);
    if (mutation == M_INDEX_MARK) irt_clearphi(f->ir[index].t);
    else if (mutation == M_INDEX_MODE) f->ir[index].op2 = IRSLOAD_TYPECHECK;
    else if (mutation == M_ALIAS_PHI_TYPE) f->ir[alias].t.irt = IRT_NUM;
    else f->ir[alias].op1 = f->ir[alias].op2;
    break;
  }
  case M_CHILD_SWAP:
    if (entry0 == UINT32_MAX || entry1 == UINT32_MAX) return 0;
    if (snap_ref(f->map[entry0]) == snap_ref(f->map[entry1]) ||
        irt_type(f->ir[snap_ref(f->map[entry0])].t) !=
        irt_type(f->ir[snap_ref(f->map[entry1])].t)) return 0;
    {
      IRRef ref0 = snap_ref(f->map[entry0]);
      IRRef ref1 = snap_ref(f->map[entry1]);
      f->map[entry0] = SNAP(snap_slot(f->map[entry0]), 0, ref1);
      f->map[entry1] = SNAP(snap_slot(f->map[entry1]), 0, ref0);
    }
    break;
  case M_CHILD_NORESTORE:
    if (entry0 == UINT32_MAX || (f->map[entry0] & SNAP_NORESTORE)) return 0;
    f->map[entry0] |= SNAP_NORESTORE; break;
  case M_CHILD_MISSING: {
    uint64_t footer = snapshot_footer(&f->copy, f->child);
    const BCIns *resume = (const BCIns *)(uintptr_t)(footer>>8);
    MSize operand = find_map_entry(f->map, snap->mapofs, snap->nent,
                                   (uint8_t)footer+2u+bc_b(*resume));
    /* A copied argument may already be dead. Remove an operand read by the
    ** resume instruction, whose value must be restored even in a sparse map. */
    assert(operand != UINT32_MAX);
    remove_map_entry(f, f->child, operand); break;
  }
  case M_CHILD_WRONG_PARTITION:
    if (entry0 == UINT32_MAX || !other_partition || other_partition[0] == 0 ||
        other_partition[0] == snap_ref(f->map[entry0]) ||
        irt_type(f->ir[other_partition[0]].t) !=
        irt_type(f->ir[snap_ref(f->map[entry0])].t)) return 0;
    f->map[entry0] = SNAP(snap_slot(f->map[entry0]), 0, other_partition[0]);
    break;
  case M_CHILD_STALE:
    if (!stale_argument) return 0;
    for (unsigned a = 0; a < nargs; a++) {
      if (stale_argument[a] && f->argentry[a] != UINT32_MAX &&
          stale_argument[a] != snap_ref(f->map[f->argentry[a]]) &&
          irt_type(f->ir[stale_argument[a]].t) ==
          irt_type(f->ir[snap_ref(f->map[f->argentry[a]])].t)) {
        f->map[f->argentry[a]] = SNAP(snap_slot(f->map[f->argentry[a]]), 0,
                                      stale_argument[a]);
        return 1;
      }
    }
    return 0;
  default: assert(0);
  }
  UNUSED(snap);
  return 1;
}

static int mutate_caller(ArgFixture *f, unsigned mutation, BCReg nargs,
                         const IRRef *other_partition,
                         const IRRef *stale_argument)
{
  MSize entry0 = f->callerentry[0], entry1 = f->callerentry[1];
  switch (mutation) {
  case M_CALLER_SWAP:
    if (entry0 == UINT32_MAX || entry1 == UINT32_MAX) return 0;
    if (snap_ref(f->map[entry0]) == snap_ref(f->map[entry1]) ||
        irt_type(f->ir[snap_ref(f->map[entry0])].t) !=
        irt_type(f->ir[snap_ref(f->map[entry1])].t)) return 0;
    {
      IRRef ref0 = snap_ref(f->map[entry0]);
      IRRef ref1 = snap_ref(f->map[entry1]);
      f->map[entry0] = SNAP(snap_slot(f->map[entry0]), 0, ref1);
      f->map[entry1] = SNAP(snap_slot(f->map[entry1]), 0, ref0);
    }
    return 1;
  case M_CALLER_NORESTORE:
    if (entry0 == UINT32_MAX || (f->map[entry0] & SNAP_NORESTORE)) return 0;
    f->map[entry0] |= SNAP_NORESTORE;
    return 1;
  case M_CALLER_MISSING:
    if (entry1 == UINT32_MAX) return 0;
    remove_map_entry(f, f->caller, entry1);
    return 1;
  case M_CALLER_WRONG_PARTITION:
    if (entry0 == UINT32_MAX || !other_partition || !other_partition[0] ||
        other_partition[0] == snap_ref(f->map[entry0]) ||
        irt_type(f->ir[other_partition[0]].t) !=
        irt_type(f->ir[snap_ref(f->map[entry0])].t)) return 0;
    f->map[entry0] = SNAP(snap_slot(f->map[entry0]), 0, other_partition[0]);
    return 1;
  case M_CALLER_STALE:
    if (!stale_argument) return 0;
    for (unsigned a = 0; a < nargs; a++) {
      MSize entry = f->callerentry[a];
      if (stale_argument[a] && entry != UINT32_MAX &&
          stale_argument[a] != snap_ref(f->map[entry]) &&
          irt_type(f->ir[stale_argument[a]].t) ==
          irt_type(f->ir[snap_ref(f->map[entry])].t)) {
        f->map[entry] = SNAP(snap_slot(f->map[entry]), 0, stale_argument[a]);
        return 1;
      }
    }
    return 0;
  default:
    return 0;
  }
}

static void admission(GCtrace *T, const ArgCase *shape)
{
  ArgFixture f = {0};
  GCproto *live_root = trace_startpt_acq(T);
  GCfunc *live_leaf = NULL;
  const BCIns *call;
  unsigned children = 0;
  f.live = T; f.n = T->nins;
  if (T->ir[f.n-1u].o == IR_NOP) f.n--;
  else while (f.n > T->nk && T->ir[f.n-1u].o == IR_RENAME) f.n--;
  for (IRRef r = REF_FIRST; r < f.n; r++)
    if (T->ir[r].o == IR_EQ && T->ir[r].t.irt == (IRT_FUNC|IRT_GUARD)) {
      assert(!f.identity);
      f.identity = r; f.kfunc = T->ir[r].op2;
    }
  assert(f.identity && f.kfunc);
  live_leaf = ir_kfunc(&T->ir[f.kfunc]);
  f.root = copy_proto(live_root);
  f.leafpt = copy_proto(funcproto(live_leaf));
  f.leaf = calloc(1, sizeof(*f.leaf)); f.J = calloc(1, sizeof(*f.J));
  f.ir = calloc(T->nins+1u, sizeof(IRIns));
  f.snap = malloc(T->nsnap*sizeof(SnapShot));
  f.map = malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf && f.J && f.ir && f.snap && f.map);
  call = root_call(f.root, shape->nargs);
  (void)call;

  for (SnapNo s = 0; s < T->nsnap; s++) {
    uint64_t pcbase;
    const SnapShot *live_snap = &T->snap[s];
    MSize childpc, base;
    const BCIns *live_leafbc = proto_bc(funcproto(live_leaf));
    IRRef other[8] = {0,0,0,0}, stale[8] = {0,0,0,0};
    memcpy(&pcbase, &T->snapmap[live_snap->mapofs+live_snap->nent],
           sizeof(pcbase));
    if (!(uint8_t)pcbase) continue;
    childpc = proto_bcpos(funcproto(live_leaf),
                          (const BCIns *)(uintptr_t)(pcbase >> 8));
    if (childpc >= funcproto(live_leaf)->sizebc) continue;
    BCOp resumeop = bc_op((BCIns)la_load32_acq(
        (const uint32_t *)&live_leafbc[childpc]));
    if (resumeop < BC_ADDVN || resumeop > BC_DIVVV) continue;
    children++;
    f.child = s; base = (uint8_t)pcbase+2u; f.argbase = (BCReg)base;
    for (unsigned a = 0; a < 8; a++) f.argentry[a] = UINT32_MAX;
    for (unsigned a = 0; a < 8; a++) f.callerentry[a] = UINT32_MAX;
    reset_fixture(&f);
    for (unsigned a = 0; a < shape->nargs; a++)
      f.argentry[a] = find_map_entry(f.map, f.snap[s].mapofs,
                                     f.snap[s].nent, base+a);
    for (unsigned a = 0; a < shape->nargs; a++)
      assert(f.argentry[a] != UINT32_MAX);

    /* Mirror the production proof: select the nearest earlier caller map
    ** whose footer is this exact BC_CALL and whose ref is in this loop half. */
    {
      const BCIns *live_call = root_call(live_root, shape->nargs);
      MSize callpos = proto_bcpos(live_root, live_call);
      const BCIns *source_call = proto_bc(live_root)+callpos;
      BCReg caller_argbase = (BCReg)(1u+LJ_FR2+bc_a(*source_call)+1u+LJ_FR2);
      MSize source_slot[8] = {0};
      int partition = T->snap[s].ref < f.loopref;
      for (unsigned a = 0; a < shape->nargs; a++) {
        for (MSize pc = callpos; pc-- > 1u;) {
          BCIns ins = proto_bc(live_root)[pc];
          if (bc_op(ins) == BC_CGET &&
              bc_a(ins)+1u+LJ_FR2 == caller_argbase+a) {
            source_slot[a] = bc_d(ins)+1u+LJ_FR2;
            break;
          }
        }
      }
      f.caller = UINT16_MAX;
      for (SnapNo t = s; t-- > 0;) {
        uint64_t footer = snapshot_footer(T, t);
        if ((uint8_t)footer ||
            (const BCIns *)(uintptr_t)(footer>>8) != source_call ||
            ((T->snap[t].ref < f.loopref) != partition)) continue;
        f.caller = t;
        break;
      }
      assert(f.caller != UINT16_MAX);
      reset_fixture(&f);
      for (unsigned a = 0; a < shape->nargs; a++) {
        f.callerentry[a] = find_map_entry(f.map, f.snap[f.caller].mapofs,
                                           f.snap[f.caller].nent,
                                           caller_argbase+a);
        assert(f.callerentry[a] != UINT32_MAX);
      }

      /* Keep a valid same-type caller argument from the other loop half and
      ** a prior same-half root value for independent binding mutations. */
      for (SnapNo t = s; t-- > 0;) {
        uint64_t footer = snapshot_footer(T, t);
        if ((uint8_t)footer ||
            (const BCIns *)(uintptr_t)(footer>>8) != source_call ||
            ((T->snap[t].ref < f.loopref) == partition)) continue;
        for (unsigned a = 0; a < shape->nargs; a++) {
          MSize e = find_map_entry(T->snapmap, T->snap[t].mapofs,
                                   T->snap[t].nent, caller_argbase+a);
          if (e != UINT32_MAX) {
            IRRef candidate = snap_ref(T->snapmap[e]);
            IRRef current = snap_ref(f.map[f.callerentry[a]]);
            if (!other[a] && candidate < f.snap[f.caller].ref &&
                candidate != current && irt_type(T->ir[candidate].t) ==
                irt_type(T->ir[current].t)) other[a] = candidate;
          }
        }
      }
      for (SnapNo t = f.caller; t-- > 0;) {
        if ((T->snap[t].ref < f.loopref) != partition ||
            T->snap[t].ref >= f.snap[f.caller].ref) continue;
        for (unsigned a = 0; a < shape->nargs; a++) {
          if (!source_slot[a]) continue;
          MSize e = find_map_entry(T->snapmap, T->snap[t].mapofs,
                                   T->snap[t].nent, source_slot[a]);
          if (e == UINT32_MAX) continue;
          IRRef old = snap_ref(T->snapmap[e]);
          IRRef current = snap_ref(f.map[f.callerentry[a]]);
          if (old < f.snap[f.caller].ref && old != current &&
              irt_type(T->ir[old].t) ==
              irt_type(T->ir[current].t)) stale[a] = old;
        }
      }
      {
        int differs = 0;
        for (unsigned a = 0; a < shape->nargs; a++)
          if (other[a] && other[a] != snap_ref(f.map[f.callerentry[a]]) &&
              irt_type(f.ir[other[a]].t) ==
              irt_type(f.ir[snap_ref(f.map[f.callerentry[a]])].t)) differs = 1;
        if (T->snap[s].ref > f.loopref) assert(differs);
      }
      if (childpc > 3u && (!strcmp(shape->name, "param0-write") ||
          !strcmp(shape->name, "later-param-write"))) {
        unsigned written = !strcmp(shape->name, "param0-write") ? 0u : 1u;
        MSize child_entry = f.argentry[written];
        IRRef incoming = snap_ref(f.map[f.callerentry[written]]);
        IRRef current = snap_ref(f.map[child_entry]);
        assert(incoming != current &&
               irt_type(f.ir[incoming].t) == irt_type(f.ir[current].t));
      }
    }

    for (int allocated = 0; allocated < 2; allocated++) {
      reset_fixture(&f);
      assert(admit(&f, allocated));
    }

    /* Find a same-source child slot from the opposite loop partition. */
    IRRef other_child[8] = {0,0,0,0};
    IRRef stale_child[8] = {0,0,0,0};
    int part = f.snap[s].ref < f.loopref;
    for (SnapNo t = 0; t < T->nsnap; t++) {
      uint64_t otherpc;
      MSize otherpcpos;
      if (t == s) continue;
      memcpy(&otherpc, &T->snapmap[T->snap[t].mapofs+T->snap[t].nent],
             sizeof(otherpc));
      if (!(uint8_t)otherpc) continue;
      otherpcpos = proto_bcpos(funcproto(live_leaf),
          (const BCIns *)(uintptr_t)(otherpc >> 8));
      if (otherpcpos >= funcproto(live_leaf)->sizebc ||
          bc_op((BCIns)la_load32_acq((const uint32_t *)&live_leafbc[otherpcpos])) !=
            BC_ADDVV || ((T->snap[t].ref < f.loopref) == part)) continue;
      for (unsigned a = 0; a < shape->nargs; a++) {
        MSize e = find_map_entry(T->snapmap, T->snap[t].mapofs,
                                 T->snap[t].nent, base+a);
        if (e != UINT32_MAX) {
          IRRef candidate = snap_ref(T->snapmap[e]);
          MSize current_entry = find_map_entry(T->snapmap, T->snap[s].mapofs,
                                               T->snap[s].nent, base+a);
          if (current_entry != UINT32_MAX) {
            IRRef current = snap_ref(T->snapmap[current_entry]);
            if (!other_child[a] && candidate < T->snap[s].ref &&
                candidate != current && irt_type(T->ir[candidate].t) ==
                irt_type(T->ir[current].t)) other_child[a] = candidate;
          }
        }
      }
    }
    for (SnapNo t = 0; t < s; t++) {
      if ((T->snap[t].ref < f.loopref) != part ||
          T->snap[t].ref >= T->snap[s].ref) continue;
      for (unsigned a = 0; a < shape->nargs; a++) {
        MSize e = find_map_entry(T->snapmap, T->snap[t].mapofs,
                                 T->snap[t].nent, base+a);
        MSize current_entry = find_map_entry(T->snapmap, T->snap[s].mapofs,
                                             T->snap[s].nent, base+a);
        if (e == UINT32_MAX) continue;
        if (current_entry == UINT32_MAX) continue;
        IRRef ref = snap_ref(T->snapmap[e]);
        IRRef current = snap_ref(T->snapmap[current_entry]);
        if (ref < T->snap[s].ref && ref != current &&
            irt_type(T->ir[ref].t) ==
            irt_type(T->ir[current].t)) stale_child[a] = ref;
      }
    }

    for (unsigned m = 0; m < M_COUNT; m++) {
      reset_fixture(&f);
      int applied = m <= M_CHILD_STALE ?
          mutate(&f, m, shape->nargs, other_child, stale_child) :
          mutate_caller(&f, m, shape->nargs, other, stale);
      if (!applied) continue;
      if (admit(&f, 0)) {
        fprintf(stderr, "argument mutation=%u snap=%u accepted semantic\n",m,s);
        abort();
      }
      semantic_bad++; mutation_semantic[m]++;

      reset_fixture(&f);
      applied = m <= M_CHILD_STALE ?
          mutate(&f, m, shape->nargs, other_child, stale_child) :
          mutate_caller(&f, m, shape->nargs, other, stale);
      if (!applied) continue;
      if (admit(&f, 1)) {
        fprintf(stderr, "argument mutation=%u snap=%u accepted postRA\n",m,s);
        abort();
      }
      postra_bad++; mutation_postra[m]++;
    }
    reset_fixture(&f);
    assert(admit(&f, 0) && admit(&f, 1));
    for (unsigned a = 0; a+1u < shape->nargs; a++) {
      IRRef left = snap_ref(f.map[f.argentry[a]]);
      IRRef right = snap_ref(f.map[f.argentry[a+1u]]);
      RegSP x, y;
      if (irref_isk(left) || irref_isk(right) || left == right ||
          irt_type(f.ir[left].t) != irt_type(f.ir[right].t)) continue;
      x = argument_location(&f, left, s); y = argument_location(&f, right, s);
      if (ra_hasspill(regsp_spill(x)) || ra_hasspill(regsp_spill(y)) ||
          !ra_hasreg(regsp_reg(x)) || !ra_hasreg(regsp_reg(y)) || x == y) continue;
      f.ir[left].r = regsp_reg(y); f.ir[left].s = SPS_NONE;
      for (IRRef r = f.n; r < T->nins; r++)
        if (f.ir[r].o == IR_RENAME && f.ir[r].op1 == left && f.ir[r].op2 <= s) {
          f.ir[r].r = regsp_reg(y); f.ir[r].s = SPS_NONE;
        }
      assert(admit(&f, 0) && !admit(&f, 1));
      alias_bad++; postra_bad++;
      break;
    }
  }
  assert(children >= 2);
  free(f.root); free(f.leafpt); free(f.leaf); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

int main(void)
{
  static const ArgCase cases[] = {
    {"args8", "return a+b+c+d+e+f+g+h",
     "return function(fn,first,last) local sum=.5; "
     "for i=first,last do sum=sum+fn(i,i+1,i+2,i+3,i+4,i+5,i+6,i+7) end; "
     "return sum end",
     8, 15, {{268435452,268435453,0},{268435451,268435453,1}}, 2},
    {"args2-mixed", "return a+b*3",
     "return function(fn,first,last) local sum=.5; "
     "for i=first,last do sum=sum+fn(.5,i) end; return sum end",
     2, 3, {{715827882,715827883,0},{715827881,715827883,1}}, 2},
    {"args2-reordered", "return b*3+a*2",
     "return function(fn,first,last) local sum=.5; local a,b; "
     "for i=first,last do a=i; b=i+1; sum=sum+fn(a,b) end; return sum end",
     2, 4, {{429496728,429496729,0},{429496727,429496729,1}}, 2},
    {"args3-constant", "return b*2+c*3+a",
     "return function(fn,first,last) local sum=.5; local b,c; "
     "for i=first,last do b=i; c=i+1; sum=sum+fn(7,b,c) end; return sum end",
     3, 7, {{429496727,429496728,0},{429496726,429496728,1}}, 2},
    {"args4", "return d+a*2+b*3+c*4",
     "return function(fn,first,last) local sum=.5; local a,b,c,d; "
     "for i=first,last do a=i; b=i+1; c=i+2; d=i+3; "
     "sum=sum+fn(a,b,c,d) end; return sum end",
     4, 9, {{214748363,214748364,0},{214748362,214748364,1}}, 2},
    {"args2-invariant-first", "return b*3+a*2",
     "return function(fn,first,last) local sum=.5; local a=first; "
     "for i=first,last do sum=sum+fn(a,i) end; return sum end",
     2, 4, {{429496729,429496730,0},{429496728,429496731,1}}, 2},
    {"param0-write", "a=a+3; return a*2+b",
     "return function(fn,first,last) local sum=.5; local a,b; "
     "for i=first,last do a=i; b=i+1; sum=sum+fn(a,b) end; return sum end",
     2, 7, {{715827880,715827881,0},{715827879,715827881,1}}, 2},
    {"later-param-write", "b=b+3; return a+b*2",
     "return function(fn,first,last) local sum=.5; local a,b; "
     "for i=first,last do a=i; b=i+1; sum=sum+fn(a,b) end; return sum end",
     2, 6, {{715827879,715827880,0},{715827878,715827880,1}}, 2},
    {"caller-updated", "return a*2+b*3",
     "return function(fn,first,last) local sum=.5; local a,b; "
     "for i=first,last do a=i; b=i+1; a=a+1; b=b+2; "
     "sum=sum+fn(a,b) end; return sum end",
     2, 4, {{429496727,429496728,0},{429496726,429496728,1}}, 2},
    {"args0", "return 7",
     "return function(fn,first,last) local sum=.5; "
     "for i=first,last do sum=sum+fn() end; return sum end",
     0, 0, {{0}}, 0},
    {"calls-twice", "return a+b",
     "return function(fn,first,last) local sum=.5; "
     "for i=first,last do sum=sum+fn(i,i+1); sum=sum+fn(i+2,i+3) end; "
     "return sum end", 2, 0, {{0}}, 0},
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  assert(LJ_FR2 == 1);
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "assert(select(2,s:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5)))");
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    const ArgCase *c = &cases[i];
    GCtrace *T;
    install_case(L, c); T = train_case(L);
    native_range(L,T,c,9,40,0);
    for (unsigned r = 0; r < c->nranges; r++) {
      native_range(L,T,c,c->ranges[r].first,c->ranges[r].last,
                   c->ranges[r].backedge ? 2 : 1);
      native_range(L,T,c,9,40,0);
    }
    lua_gc(L,LUA_GCCOLLECT,0);
    native_range(L,T,c,1,24,0); gc_reuses++;
    if (c->nranges) admission(T,c);
    printf("Lua fixed args %s: child-exits=%u native-trace-reused=yes\n",
           c->name,c->nranges);
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  for (unsigned i = 0; i < M_COUNT; i++) {
    fprintf(stderr, "argument mutation=%u semantic=%u postRA=%u\n",
            i, mutation_semantic[i], mutation_postra[i]);
    assert(mutation_semantic[i] > 0 && mutation_postra[i] > 0);
  }
  assert(native_runs == 58 && child_exits == 18 &&
         same_trace_reuses == 40 && gc_reuses == 11 && alias_bad > 0);
  printf("Lua fixed args OK: native=%u child-exits=%u same-trace=%u gc=%u "
         "semantic-negative=%u postRA-negative=%u aliases=%u cold-retries=%u\n",
         native_runs, child_exits, same_trace_reuses, gc_reuses,
         semantic_bad, postra_bad, alias_bad, cold_retries);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-arguments SKIP"); return 0; }
#endif
