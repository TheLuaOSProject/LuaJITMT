/* Normalized fixed Lua arguments, native replay and child restoration. */
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

typedef struct NormCase {
  const char *name, *params, *body, *args, *types;
  BCReg nparams;
  BCOp exitop;
  unsigned exitoccurrence, overflow;
  unsigned returns_value;  /* 1: payload; 2: omitted nil parameter. */
} NormCase;

typedef struct NormFixture {
  GCtrace *live, copy;
  GCproto *root, *leafpt;
  GCfunc *leaf;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef n, kfunc, loopref;
  SnapNo child;
  BCReg childbase;
} NormFixture;

static unsigned native_runs, child_exits, same_trace_reuses, gc_reuses;
static unsigned identity_exits, observer_calls, cases_run;
static unsigned semantic_bad, postra_bad, aliases, missing_nil;
static unsigned full_actuals, normalized_children, raw_old_values;
static unsigned mutation_count[11][2], cdata_return_refusals;
static const NormCase *active_case;

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
  lua_getglobal(L, "arg_payload");
  ljt_lua_pcall(L, 4, 1, "Lua normalized arguments");
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  if (active_case->returns_value)
    assert(result == .5+last-first+1);  /* Each returned value matched. */
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

static const BCIns *arithmetic_pc(GCproto *pt, BCOp op, unsigned occurrence)
{
  const BCIns *bc = proto_bc(pt);
  for (MSize pc = 1; pc < pt->sizebc; pc++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pc]);
    if (bc_op(ins) == op && occurrence-- == 0) return &bc[pc];
  }
  assert(0 && "selected arithmetic bytecode missing");
  return NULL;
}

static void assert_child_exit(lua_State *L, GCtrace *T,
                              const NormCase *shape, int backedge)
{
  GCproto *runpt = funcproto(global_function(L, "leaf_run"));
  GCfunc *leaf = global_function(L, "leaf_candidate");
  GCproto *leafpt = funcproto(leaf);
  const BCIns *call = only_call(runpt);
  const BCIns *childpc = arithmetic_pc(leafpt, shape->exitop,
                                       shape->exitoccurrence);
  ExitNo exitno = lj_trace_test_first_exitno();
  const SnapShot *snap;
  const SnapEntry *map;
  IRRef loop = 0;
  MSize ofs, nent;
  uint64_t pcbase;
  BCReg frame_slot, childbase;
  int found_frame = 0, found_func = 0;

  assert(bc_op((BCIns)la_load32_acq((const uint32_t *)call)) == BC_CALL &&
         bc_b((BCIns)la_load32_acq((const uint32_t *)call)) == 2 &&
         bc_c((BCIns)la_load32_acq((const uint32_t *)call)) == strlen(shape->types)+1u &&
         leafpt->numparams == shape->nparams);
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
  childbase = (BCReg)((uint8_t)pcbase+2u);
  assert((uint8_t)pcbase == (uint8_t)(bc_a((BCIns)
      la_load32_acq((const uint32_t *)call)) + 1u + LJ_FR2));
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
    if (snap_slot(entry) >= childbase &&
        snap_slot(entry) < childbase+leafpt->framesize)
      assert(!(entry & (SNAP_NORESTORE|SNAP_FRAME)) &&
             irt_type(ir->t) != IRT_P32 && irt_type(ir->t) != IRT_PGC);
  }
  assert(found_frame && found_func);
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exitno() == exitno &&
         lj_trace_test_first_exit_parent() == T->traceno);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void native_range(lua_State *L, GCtrace *T, const NormCase *shape,
                         lua_Integer first, lua_Integer last, int childexit)
{
  lua_Number expected = invoke_range(L, "leaf_oracle_run", "leaf_oracle",
                                    first, last);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(invoke_range(L, "leaf_run", "leaf_candidate", first, last) == expected);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno);
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  if (childexit) {
    assert_child_exit(L, T, shape, childexit == 2);
    child_exits++;
  } else same_trace_reuses++;
  native_runs++;
}

static void install_case(lua_State *L, const NormCase *shape)
{
  char source[4096], runner[1024];
  active_case = shape;
  if (shape->returns_value)
    snprintf(runner, sizeof(runner),
      "return function(fn,first,last,payload) local sum=.5; "
      "for i=first,last do local value=fn(%s); "
      "if value~=%s then return -1 end; sum=sum+1 end; return sum end",
      shape->args, shape->returns_value == 2 ? "nil" : "payload");
  else
    snprintf(runner, sizeof(runner),
      "return function(fn,first,last,payload) local sum=.5; "
      "for i=first,last do sum=sum+fn(%s) end; return sum end", shape->args);
  snprintf(source, sizeof(source),
    "leaf_candidate=assert(loadstring('return function(%s) %s end',"
      "'@normalized-candidate-%s'))(); "
    "leaf_oracle=assert(loadstring('return function(%s) %s end',"
      "'@normalized-oracle-%s'))(); "
    "leaf_run=assert(loadstring('%s','@normalized-run-%s'))(); "
    "leaf_oracle_run=assert(loadstring('%s','@normalized-oracle-run-%s'))(); "
    "jit.off(leaf_oracle,true); jit.off(leaf_oracle_run,true); "
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000',"
      "'maxtrace=64')",
    shape->params,shape->body,shape->name,shape->params,shape->body,shape->name,
    runner,shape->name,runner,shape->name);
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
  }
  assert(T != NULL);
  return T;
}

static GCproto *copy_proto(const GCproto *pt)
{
  GCproto *copy = malloc(pt->sizept);
  assert(copy != NULL);
  memcpy(copy, pt, pt->sizept);
  return copy;
}

static uint64_t snapshot_footer(const GCtrace *T, SnapNo s)
{
  const SnapShot *snap = &T->snap[s];
  uint64_t footer;
  memcpy(&footer, &T->snapmap[snap->mapofs+snap->nent], sizeof(footer));
  return footer;
}

static MSize map_entry(const SnapEntry *map, MSize ofs, MSize nent,
                       BCReg slot)
{
  for (MSize i = 0; i < nent; i++)
    if (snap_slot(map[ofs+i]) == slot) return ofs+i;
  return UINT32_MAX;
}

static void reset_fixture(NormFixture *f)
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
    if (f->ir[r].o == IR_LOOP) f->loopref = f->J->loopref = r;
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

static int admit(NormFixture *f, int allocated)
{
  if (allocated) return lj_asm_arm64_postra_admit(&f->view, NULL);
  f->copy.nins = f->n;
  return lj_asm_arm64_ir_admit(f->J, &f->copy, NULL);
}

static void remove_entry(NormFixture *f, SnapNo sn, MSize entry)
{
  SnapShot *s = &f->snap[sn];
  assert(entry >= s->mapofs && entry < s->mapofs+s->nent);
  memmove(&f->map[entry], &f->map[entry+1u],
          (f->copy.nsnapmap-entry-1u)*sizeof(SnapEntry));
  s->nent--;
  for (SnapNo i = sn+1u; i < f->copy.nsnap; i++) f->snap[i].mapofs--;
  f->copy.nsnapmap--; f->view.nsnapmap--;
}

static RegSP value_location(const NormFixture *f, IRRef ref, SnapNo snapno)
{
  RegSP rs = f->ir[ref].prev;
  for (IRRef r = f->live->nins; r-- > f->n;)
    if (f->ir[r].o == IR_RENAME && f->ir[r].op1 == ref &&
        f->ir[r].op2 <= snapno) rs = f->ir[r].prev;
  return rs;
}

/* A different callee observes the original CALL after the native identity
** guard fails. Even ignored extra arguments must survive with exact values. */
static int observe_arguments(lua_State *L)
{
  unsigned count = (unsigned)strlen(active_case->types);
  unsigned number = 0;
  assert(lua_gettop(L) == (int)count);
  for (unsigned i = 0; i < count; i++) {
    switch (active_case->types[i]) {
    case 'n':
      assert(lua_type(L, i+1u) == LUA_TNUMBER &&
             lua_tonumber(L, i+1u) == 9+observer_calls+number);
      number++;
      break;
    case '0': assert(lua_isnil(L, i+1u)); break;
    case 't': assert(lua_isboolean(L, i+1u) && lua_toboolean(L, i+1u)); break;
    case 'f': assert(lua_isboolean(L, i+1u) && !lua_toboolean(L, i+1u)); break;
    case 'p':
      lua_getglobal(L, "arg_payload");
      assert(lua_rawequal(L, i+1u, -1));
      lua_pop(L, 1);
      break;
    default: assert(0);
    }
  }
  observer_calls++;
  if (active_case->returns_value == 2) lua_pushnil(L);
  else if (active_case->returns_value) lua_getglobal(L, "arg_payload");
  else lua_pushnumber(L, 123+count);
  return 1;
}

static void identity_range(lua_State *L, GCtrace *T)
{
  lua_Number expected;
  uint64_t footer;
  observer_calls = 0;
  expected = invoke_range(L, "leaf_oracle_run", "arg_observer", 9, 40);
  assert(observer_calls == 32);
  observer_calls = 0;
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke_range(L, "leaf_run", "arg_observer", 9, 40) == expected);
  assert(observer_calls == 32 && lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno && root_trace(L) == T);
  footer = snapshot_footer(T, lj_trace_test_first_exitno());
  assert(!(uint8_t)footer &&
         (const BCIns *)(uintptr_t)(footer>>8) == only_call(trace_startpt_acq(T)));
  native_runs++; identity_exits++;
}

static void admit_negative(NormFixture *f, unsigned mutation, int allocated)
{
  if (admit(f, allocated)) {
    fprintf(stderr, "normalized %s mutation=%u snapshot=%u accepted %s\n",
            active_case->name, mutation, f->child,
            allocated ? "postRA" : "semantic");
    abort();
  }
  mutation_count[mutation][allocated]++;
  if (allocated) postra_bad++; else semantic_bad++;
}

static void admission(GCtrace *T, const NormCase *shape)
{
  NormFixture f = {0};
  GCfunc *leaf;
  GCproto *pt;
  unsigned actual = (unsigned)strlen(shape->types);
  f.live = T; f.n = T->nins;
  if (T->ir[f.n-1u].o == IR_NOP) f.n--;
  else while (f.n > T->nk && T->ir[f.n-1u].o == IR_RENAME) f.n--;
  for (IRRef r = REF_FIRST; r < f.n; r++)
    if (T->ir[r].o == IR_EQ && T->ir[r].t.irt == (IRT_FUNC|IRT_GUARD)) {
      assert(!f.kfunc); f.kfunc = T->ir[r].op2;
    }
  assert(f.kfunc);
  leaf = ir_kfunc(&T->ir[f.kfunc]); pt = funcproto(leaf);
  f.root = copy_proto(trace_startpt_acq(T)); f.leafpt = copy_proto(pt);
  f.leaf = calloc(1, sizeof(*f.leaf)); f.J = calloc(1, sizeof(*f.J));
  f.ir = calloc(T->nins+1u, sizeof(IRIns));
  f.snap = malloc(T->nsnap*sizeof(SnapShot));
  f.map = malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf && f.J && f.ir && f.snap && f.map);
  for (SnapNo s = 0; s < T->nsnap; s++) {
    uint64_t footer = snapshot_footer(T, s);
    const BCIns *pc = (const BCIns *)(uintptr_t)(footer>>8);
    unsigned base;
    f.child = s;
    reset_fixture(&f);
    assert(admit(&f, 0) && admit(&f, 1));
    if (!(uint8_t)footer) {
      IRRef at = T->snap[s].ref;
      int identity = T->ir[at].o == IR_EQ && T->ir[at].op2 == f.kfunc;
      int child = s+1u < T->nsnap && (uint8_t)snapshot_footer(T, s+1u);
      if (pc != only_call(trace_startpt_acq(T))) continue;
      /* A guard-elided map with no child snapshot is never replayed and
      ** supplies no child proof. It has no required argument authority. */
      if (!identity && !child) continue;
      base = 2u+2u*LJ_FR2+bc_a(*pc);
      for (unsigned a = 0; a < actual; a++) {
        for (int allocated = 0; allocated < 2; allocated++) {
          MSize entry;
          reset_fixture(&f);
          entry = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent, base+a);
          assert(entry != UINT32_MAX);
          remove_entry(&f, s, entry);
          admit_negative(&f, 0, allocated);
          reset_fixture(&f);
          f.map[entry] |= SNAP_NORESTORE;
          admit_negative(&f, 1, allocated);
        }
        full_actuals++;
      }
      for (int allocated = 0; allocated < 2; allocated++) {
        for (unsigned mutation = 2; mutation <= 4; mutation++) {
          BCIns *call;
          reset_fixture(&f);
          call = (BCIns *)(uintptr_t)only_call(f.root);
          if (mutation == 2) setbc_c(call, 0);
          else if (mutation == 3) setbc_c(call, UINT8_MAX);
          else setbc_b(call, 0);
          admit_negative(&f, mutation, allocated);
        }
      }
      continue;
    }
    base = (uint8_t)footer+2u;
    normalized_children++;
    /* Each actual variable operand at a child resume must still be present. */
    for (int allocated = 0; allocated < 2; allocated++) {
      MSize entry;
      reset_fixture(&f);
      entry = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent, base+bc_b(*pc));
      assert(entry != UINT32_MAX);
      remove_entry(&f, s, entry);
      admit_negative(&f, 5, allocated);
      if (bc_op(*pc) >= BC_ADDVV && bc_op(*pc) <= BC_DIVVV) {
        reset_fixture(&f);
        entry = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                          base+bc_c(*pc));
        assert(entry != UINT32_MAX);
        remove_entry(&f, s, entry);
        admit_negative(&f, 10, allocated);
      }
      reset_fixture(&f);
      f.snap[s].nslots--;
      admit_negative(&f, 6, allocated);
      reset_fixture(&f);
      f.leafpt->numparams = f.leafpt->framesize+1u;
      admit_negative(&f, 7, allocated);
    }
    /* Missing b is nil before the overflowing assignment, and CSET still
    ** reads its old tag. False or an absent entry cannot stand in for nil. */
    if (!strcmp(shape->name, "missing-write")) {
      for (int allocated = 0; allocated < 2; allocated++) {
        MSize entry;
        reset_fixture(&f);
        entry = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent, base+1u);
        if (entry == UINT32_MAX || snap_ref(f.map[entry]) != REF_NIL) continue;
        f.map[entry] = SNAP(base+1u, 0, REF_FALSE);
        admit_negative(&f, 8, allocated);
        reset_fixture(&f);
        remove_entry(&f, s, entry);
        admit_negative(&f, 9, allocated);
        missing_nil++;
      }
    }
    reset_fixture(&f);
    for (MSize i = 0; i < f.snap[s].nent; i++) {
      SnapEntry sn = f.map[f.snap[s].mapofs+i];
      IRRef r = snap_ref(sn);
      IRType type = irt_type(f.ir[r].t);
      if (snap_slot(sn) < base || type == IRT_INT || type == IRT_NUM) continue;
      raw_old_values++;
      if (irref_isk(r)) continue;
      for (MSize j = 0; j < f.snap[s].nent; j++) {
        IRRef other = snap_ref(f.map[f.snap[s].mapofs+j]);
        RegSP from, to;
        if (irref_isk(other) || other == r || irt_type(f.ir[other].t) != IRT_INT)
          continue;
        from = value_location(&f, r, s); to = value_location(&f, other, s);
        if (!ra_hasreg(regsp_reg(from)) || !ra_hasreg(regsp_reg(to)) ||
            ra_hasspill(regsp_spill(from)) || ra_hasspill(regsp_spill(to)) ||
            regsp_reg(from) == regsp_reg(to)) continue;
        f.ir[r].r = regsp_reg(to); f.ir[r].s = SPS_NONE;
        for (IRRef ren = f.n; ren < T->nins; ren++)
          if (f.ir[ren].o == IR_RENAME && f.ir[ren].op1 == r && f.ir[ren].op2 <= s) {
            f.ir[ren].r = regsp_reg(to); f.ir[ren].s = SPS_NONE;
          }
        assert(admit(&f, 0) && !admit(&f, 1));
        aliases++; postra_bad++;
        goto next_snapshot;
      }
    }
next_snapshot:;
  }
  free(f.root); free(f.leafpt); free(f.leaf); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

static void run_case(lua_State *L, const NormCase *shape)
{
  GCtrace *T;
  lua_Integer safe = shape->exitop == BC_ADDVV ? 1073741823 : 715827882;
  install_case(L, shape); T = train_case(L);
  native_range(L, T, shape, 9, 40, 0);
  if (shape->overflow) {
    native_range(L, T, shape, safe, safe+1, 1);
    native_range(L, T, shape, 9, 40, 0);
    native_range(L, T, shape, safe-1, safe+1, 2);
    native_range(L, T, shape, 9, 40, 0);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  native_range(L, T, shape, 1, 24, 0); gc_reuses++;
  identity_range(L, T);
  native_range(L, T, shape, 9, 40, 0);
  admission(T, shape);
  cases_run++;
  printf("Lua normalized %s: actual=%zu formals=%u native exits/reuse verified\n",
         shape->name, strlen(shape->types), shape->nparams);
}

int main(void)
{
  static const NormCase cases[] = {
    {"extra-numeric","a,b","return a+b","i,i+1,i+2","nnn",2,BC_ADDVV,0,1,0},
    {"extra-primitives","a,b","return a+b","i,i+1,nil,true,false","nn0tf",2,BC_ADDVV,0,1,0},
    {"missing-tail","a,b","return a*3","i","n",2,BC_MULVN,0,1,0},
    {"missing-many","a,b,c,d,e,f,g,h","return a*3","i","n",8,BC_MULVN,0,1,0},
    {"missing-all","a,b,c","return 7","","",3,BC_KSHORT,0,0,0},
    {"nil-middle","a,b,c","return a+c","i,nil,i+1","n0n",3,BC_ADDVV,0,1,0},
    {"true-middle","a,b,c","return a+c","i,true,i+1","ntn",3,BC_ADDVV,0,1,0},
    {"false-middle","a,b,c","return a+c","i,false,i+1","nfn",3,BC_ADDVV,0,1,0},
    {"missing-write","a,b","b=a*3; return b+1","i","n",2,BC_MULVN,0,1,0},
    {"zero-formals","","return 7","i,nil,true","n0t",0,BC_KSHORT,0,0,0},
    {"return-missing","a,b","return b","i","n",2,BC_CGET,0,0,2}
  };
  static const char *payloads[] = {
    "nil", "false", "true", "7", ".5", "string.rep('payload',100)",
    "{}", "function() return 7 end", "coroutine.create(function() end)",
    "newproxy(true)", "require('ffi').new('int[1]',17)", NULL
  };
  static const char *names[] = {
    "nil", "false", "true", "int", "num", "string", "table", "function",
    "thread", "userdata", "cdata", "lightuserdata"
  };
  static const NormCase layouts[] = {
    {"unused","a,b,c","return a+c","i,payload,i+1","npn",3,BC_ADDVV,0,1,0},
    {"extra","a,b","return a+b","i,i+1,payload","nnp",2,BC_ADDVV,0,1,0},
    {"overwrite","a,b","b=a*3; return b+1","i,payload","np",2,BC_MULVN,0,1,0},
    {"source-value","a,b,c","c=a; a=b; return c*3","i,payload","np",3,BC_MULVN,0,1,0},
    {"return-value","a,b","return b","i,payload","np",2,BC_CGET,0,0,1}
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  lua_pushcfunction(L, observe_arguments); lua_setglobal(L, "arg_observer");
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "assert(select(2,s:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5)))");
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) run_case(L, &cases[i]);
  for (unsigned p = 0; p < sizeof(payloads)/sizeof(payloads[0]); p++) {
    char source[256];
    if (payloads[p]) {
      snprintf(source, sizeof(source), "arg_payload=%s", payloads[p]);
      ljt_lua_dostring(L, source);
    } else {
      lua_pushlightuserdata(L, (void *)&native_runs); lua_setglobal(L, "arg_payload");
    }
    for (unsigned k = 0; k < sizeof(layouts)/sizeof(layouts[0]); k++) {
      NormCase shape = layouts[k];
      char name[64];
      snprintf(name, sizeof(name), "%s-%s", shape.name, names[p]);
      shape.name = name;
      if (shape.returns_value && !strcmp(names[p], "cdata")) {
        /* The cdata equality consumer is still outside the root grammar;
        ** the other four layouts exercise cdata normalization natively. */
        install_case(L, &shape);
        for (unsigned attempt = 0; attempt < 8; attempt++) {
          lua_Number expected = invoke_range(L, "leaf_oracle_run", "leaf_oracle", 1, 96);
          assert(invoke_range(L, "leaf_run", "leaf_candidate", 1, 96) == expected);
          assert(root_trace(L) == NULL);
        }
        cdata_return_refusals++;
        continue;
      }
      run_case(L, &shape);
    }
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  for (unsigned m = 0; m < 11; m++)
    assert(mutation_count[m][0] && mutation_count[m][1]);
  assert(cases_run == 70 && native_runs == 504 && child_exits == 112 &&
         identity_exits == 70 && same_trace_reuses == 322 && gc_reuses == 70 &&
         cdata_return_refusals == 1 &&
         missing_nil && aliases && full_actuals && normalized_children && raw_old_values);
  printf("Lua normalized args OK: cases=%u native=%u child=%u identity=%u reuse=%u gc=%u "
         "semantic-negative=%u postRA-negative=%u aliases=%u missing-nil=%u "
         "full-actuals=%u children=%u raw-values=%u cdata-equality-refusals=%u\n", cases_run,native_runs,child_exits,
         identity_exits,same_trace_reuses,gc_reuses,semantic_bad,postra_bad,aliases,
         missing_nil,full_actuals,normalized_children,raw_old_values,cdata_return_refusals);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-normalized-args SKIP"); return 0; }
#endif

