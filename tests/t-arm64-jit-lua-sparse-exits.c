/* Sparse child-frame snapshots for fixed-arity numeric Lua leaves. */
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

typedef struct SparseCase {
  const char *name, *body, *runner;
  BCReg nargs;
  BCOp exitop;
  unsigned exitoccurrence;
  unsigned nranges;
  struct { lua_Integer first, last; int backedge; } ranges[2];
  /* Source-frame slots proved dead at the selected arithmetic resume PC. */
  BCReg dead_slot[2];
  unsigned ndead;
  /* Unused parameters which must already be absent in the recorded map. */
  BCReg hole_first, hole_count;
  /* A later CSET writes this already-defined local; its old value is read. */
  int cset_old_slot;
} SparseCase;

typedef struct SparseFixture {
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
} SparseFixture;

static unsigned native_runs, child_exits, same_trace_reuses, gc_reuses;
static unsigned positive_omissions, semantic_bad, postra_bad, alias_bad;
static unsigned negative_live[4];
static unsigned omission_by_case[4];
static unsigned natural_holes;
static unsigned callmap_only, executable_locations, boundary_bad;

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
  ljt_lua_pcall(L, 3, 1, "Lua sparse child exit");
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
                              const SparseCase *shape, int backedge)
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
             (irt_type(ir->t) == IRT_INT || irt_type(ir->t) == IRT_NUM));
  }
  assert(found_frame && found_func);
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exitno() == exitno &&
         lj_trace_test_first_exit_parent() == T->traceno);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void native_range(lua_State *L, GCtrace *T, const SparseCase *shape,
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

static void install_case(lua_State *L, const SparseCase *shape)
{
  char source[4096];
  const char *params = shape->nargs == 8 ? "a,b,c,d,e,f,g,h" :
                       shape->nargs == 3 ? "a,b,c" : "a,b";
  snprintf(source, sizeof(source),
    "leaf_candidate=assert(loadstring('return function(%s) %s end',"
      "'@sparse-candidate-%s'))(); "
    "leaf_oracle=assert(loadstring('return function(%s) %s end',"
      "'@sparse-oracle-%s'))(); "
    "leaf_run=assert(loadstring('%s','@sparse-run-%s'))(); "
    "leaf_oracle_run=assert(loadstring('%s','@sparse-oracle-run-%s'))(); "
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

static void reset_fixture(SparseFixture *f)
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

static int admit(SparseFixture *f, int allocated)
{
  if (allocated) return lj_asm_arm64_postra_admit(&f->view, NULL);
  f->copy.nins = f->n;
  return lj_asm_arm64_ir_admit(f->J, &f->copy, NULL);
}

static void remove_entry(SparseFixture *f, SnapNo sn, MSize entry)
{
  SnapShot *s = &f->snap[sn];
  assert(entry >= s->mapofs && entry < s->mapofs+s->nent);
  memmove(&f->map[entry], &f->map[entry+1u],
          (f->copy.nsnapmap-entry-1u)*sizeof(SnapEntry));
  s->nent--;
  for (SnapNo i = sn+1u; i < f->copy.nsnap; i++) f->snap[i].mapofs--;
  f->copy.nsnapmap--; f->view.nsnapmap--;
}

static RegSP value_location(const SparseFixture *f, IRRef ref, SnapNo snapno)
{
  RegSP rs = f->ir[ref].prev;
  for (IRRef r = f->live->nins; r-- > f->n;)
    if (f->ir[r].o == IR_RENAME && f->ir[r].op1 == ref &&
        f->ir[r].op2 <= snapno) rs = f->ir[r].prev;
  return rs;
}

/* Exercise actual child snapshots only. The validator must keep exact refs
** for live slots, while a dropped slot is accepted only when future bytecode
** proves it dead. */
static void admission(GCtrace *T, const SparseCase *shape)
{
  SparseFixture f = {0};
  GCproto *root = trace_startpt_acq(T);
  GCfunc *leaf;
  GCproto *leafpt;
  f.live = T; f.n = T->nins;
  if (T->ir[f.n-1u].o == IR_NOP) f.n--;
  else while (f.n > T->nk && T->ir[f.n-1u].o == IR_RENAME) f.n--;
  for (IRRef r = REF_FIRST; r < f.n; r++)
    if (T->ir[r].o == IR_EQ && T->ir[r].t.irt == (IRT_FUNC|IRT_GUARD)) {
      assert(!f.kfunc); f.kfunc = T->ir[r].op2;
    }
  assert(f.kfunc);
  leaf = ir_kfunc(&T->ir[f.kfunc]); leafpt = funcproto(leaf);
  f.root = copy_proto(root); f.leafpt = copy_proto(leafpt);
  f.leaf = calloc(1, sizeof(*f.leaf)); f.J = calloc(1, sizeof(*f.J));
  f.ir = calloc(T->nins+1u, sizeof(IRIns));
  f.snap = malloc(T->nsnap*sizeof(SnapShot));
  f.map = malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf && f.J && f.ir && f.snap && f.map);
  for (SnapNo s = 0; s < T->nsnap; s++) {
    uint64_t footer = snapshot_footer(T, s);
    if (!(uint8_t)footer) continue;
    const BCIns *pc = (const BCIns *)(uintptr_t)(footer >> 8);
    const BCIns *target = arithmetic_pc(leafpt, shape->exitop,
                                        shape->exitoccurrence);
    if (pc != target)
      continue;
    f.child = s;
    f.childbase = (BCReg)((uint8_t)footer+2u);
    reset_fixture(&f);
    assert(admit(&f, 0) && admit(&f, 1));
    for (unsigned h = 0; h < shape->hole_count; h++) {
      assert(map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
        f.childbase+shape->hole_first+h) == UINT32_MAX);
      natural_holes++;
    }

    /* Drop a genuinely present source-dead slot. Try each declared dead
    ** slot and count every accepted semantic/post-RA clone. */
    for (unsigned d = 0; d < shape->ndead; d++) {
      reset_fixture(&f);
      MSize e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                          f.childbase+shape->dead_slot[d]);
      if (e == UINT32_MAX) continue;
      for (int allocated = 0; allocated < 2; allocated++) {
        uint8_t nslots, topslot;
        reset_fixture(&f);
        nslots = f.snap[s].nslots; topslot = f.snap[s].topslot;
        e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                      f.childbase+shape->dead_slot[d]);
        assert(e != UINT32_MAX);
        remove_entry(&f, s, e);
        assert(f.snap[s].nslots == nslots && f.snap[s].topslot == topslot);
        assert(admit(&f, allocated));
        positive_omissions++;
        if (!strcmp(shape->name, "unused8")) omission_by_case[0]++;
        else if (!strcmp(shape->name, "unused-middle3")) omission_by_case[1]++;
        else if (!strcmp(shape->name, "unused-tail")) omission_by_case[2]++;
        else if (!strcmp(shape->name, "dead-local")) omission_by_case[3]++;
      }
    }

    /* The source parameters may already have been copied. Remove each
    ** temporary actually read by the resume instruction. */
    for (unsigned operand = 0; operand < (bc_op(*target) == BC_ADDVV ? 2u : 1u);
         operand++) {
      BCReg slot = operand ? bc_c(*target) : bc_b(*target);
      MSize e;
      reset_fixture(&f);
      e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                    f.childbase+slot);
      assert(e != UINT32_MAX);
      remove_entry(&f, s, e);
      assert(!admit(&f, 0)); semantic_bad++; negative_live[0]++;
      reset_fixture(&f);
      e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                    f.childbase+slot);
      assert(e != UINT32_MAX);
      remove_entry(&f, s, e);
      assert(!admit(&f, 1)); postra_bad++; negative_live[0]++;
    }

    /* The full caller map is still authoritative even for an argument that
    ** is absent from the child. Only an equal-ref, immediately following
    ** child map can make that caller map nonexecutable. */
    if (shape->hole_count) {
      SnapNo caller = s;
      MSize argument;
      while (caller && (uint8_t)snapshot_footer(T, --caller)) {}
      assert(caller && s == caller+1u);
      reset_fixture(&f);
      argument = map_entry(f.map, f.snap[caller].mapofs, f.snap[caller].nent,
                            f.childbase+shape->hole_first);
      assert(argument != UINT32_MAX);
      for (int allocated = 0; allocated < 2; allocated++) {
        reset_fixture(&f);
        remove_entry(&f, caller, argument);
        assert(!admit(&f, allocated));
        if (allocated) postra_bad++; else semantic_bad++;
      }
      reset_fixture(&f);
      if (f.snap[caller].ref == f.snap[s].ref) {
        if (!strcmp(shape->name, "unused-tail")) {
          IRRef unused = snap_ref(f.map[argument]);
          RegSP rs = value_location(&f, unused, caller);
          assert(!irref_isk(unused) && ra_noreg(regsp_reg(rs)) &&
                 !ra_hasspill(regsp_spill(rs)));
          assert(admit(&f, 0) && admit(&f, 1));
          callmap_only++;
        }
      } else {
        IRRef unused = snap_ref(f.map[argument]);
        assert(f.snap[caller].ref < f.snap[s].ref && !irref_isk(unused));
        f.ir[unused].r = RID_INIT; f.ir[unused].s = SPS_NONE;
        for (IRRef r = f.n; r < T->nins; r++)
          if (f.ir[r].o == IR_RENAME && f.ir[r].op1 == unused)
            f.ir[r].r = RID_INIT;
        assert(admit(&f, 0) && !admit(&f, 1));
        postra_bad++; executable_locations++;
      }
    }

    for (int allocated = 0; allocated < 2; allocated++) {
      for (unsigned mutation = 0; mutation < 5; mutation++) {
        uint64_t changed;
        reset_fixture(&f);
        if (mutation == 0) f.snap[s].nslots--;
        else if (mutation == 1) f.snap[s].topslot--;
        else if (mutation == 2) f.snap[s].ref = f.loopref;
        else {
          changed = snapshot_footer(&f.copy, s);
          if (mutation == 3) changed &= ~UINT64_C(255);
          else changed = (u64ptr(proto_bc(f.leafpt)+f.leafpt->sizebc-1u)<<8)|
                         (uint8_t)changed;
          memcpy(&f.map[f.snap[s].mapofs+f.snap[s].nent], &changed, sizeof(changed));
        }
        assert(!admit(&f, allocated));
        if (allocated) postra_bad++; else semantic_bad++;
        boundary_bad++;
      }
    }

    /* The second parameter in this leaf is live after its first overflow. */
    if (!strcmp(shape->name, "later-param")) {
      MSize e;
      reset_fixture(&f);
      e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                    f.childbase+1u);
      assert(e != UINT32_MAX);
      remove_entry(&f, s, e);
      assert(!admit(&f, 0)); semantic_bad++; negative_live[2]++;
      reset_fixture(&f);
      e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                    f.childbase+1u);
      assert(e != UINT32_MAX);
      remove_entry(&f, s, e);
      assert(!admit(&f, 1)); postra_bad++; negative_live[2]++;
    }

    /* CSET reads the old destination tag even after its numeric value has
    ** been copied to the arithmetic temporary. */
    if (shape->cset_old_slot >= 0) {
      MSize e;
      reset_fixture(&f);
      e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                    f.childbase+(BCReg)shape->cset_old_slot);
      if (e != UINT32_MAX) {
        remove_entry(&f, s, e);
        assert(!admit(&f, 0)); semantic_bad++; negative_live[1]++;
        reset_fixture(&f);
        e = map_entry(f.map, f.snap[s].mapofs, f.snap[s].nent,
                      f.childbase+(BCReg)shape->cset_old_slot);
        assert(e != UINT32_MAX);
        remove_entry(&f, s, e);
        assert(!admit(&f, 1)); postra_bad++; negative_live[1]++;
      }
    }

    /* Distinct live values may not share one physical register in the child
    ** exit map. Preserve their IR identities and mutate only allocation. */
    reset_fixture(&f);
    for (MSize a = 0; a < f.snap[s].nent; a++) {
      SnapEntry ea = f.map[f.snap[s].mapofs+a];
      IRRef ra = snap_ref(ea);
      if (snap_slot(ea) < f.childbase || snap_slot(ea) >= f.childbase+leafpt->framesize ||
          ra >= f.n || irref_isk(ra)) continue;
      for (MSize b = a+1u; b < f.snap[s].nent; b++) {
        SnapEntry eb = f.map[f.snap[s].mapofs+b];
        IRRef rb = snap_ref(eb);
        RegSP la, lb;
        if (snap_slot(eb) < f.childbase || snap_slot(eb) >= f.childbase+leafpt->framesize ||
            rb >= f.n || irref_isk(rb) || ra == rb ||
            irt_type(f.ir[ra].t) != irt_type(f.ir[rb].t)) continue;
        la = value_location(&f, ra, s); lb = value_location(&f, rb, s);
        if (la == lb || !ra_hasreg(regsp_reg(la)) ||
            !ra_hasreg(regsp_reg(lb)) || ra_hasspill(regsp_spill(la)) ||
            ra_hasspill(regsp_spill(lb))) continue;
        assert(admit(&f, 0));
        f.ir[ra].r = regsp_reg(lb); f.ir[ra].s = SPS_NONE;
        for (IRRef r = f.n; r < T->nins; r++)
          if (f.ir[r].o == IR_RENAME && f.ir[r].op1 == ra &&
              f.ir[r].op2 <= s) {
            f.ir[r].r = regsp_reg(lb); f.ir[r].s = SPS_NONE;
          }
        assert(admit(&f, 0) && !admit(&f, 1));
        postra_bad++; alias_bad++;
        goto alias_done;
      }
    }
alias_done:
    ;
  }
  free(f.root); free(f.leafpt); free(f.leaf); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

int main(void)
{
  static const SparseCase cases[] = {
    {"unused8", "return a+h",
     "return function(fn,first,last) local sum=.5; for i=first,last do "
       "sum=sum+fn(i,i+1,i+2,i+3,i+4,i+5,i+6,i+7) end; return sum end",
     8, BC_ADDVV, 0, 2,
     {{1073741820,1073741821,0},{1073741819,1073741821,1}},
     {0,7}, 2, 1,6, -1},
    {"unused-middle3", "return a+c",
     "return function(fn,first,last) local sum=.5; for i=first,last do "
       "sum=sum+fn(i,i+1,i+2) end; return sum end",
     3, BC_ADDVV, 0, 2, {{1073741822,1073741823,0},{1073741821,1073741823,1}},
     {0,2}, 2, 1,1, -1},
    {"unused-tail", "return a+b",
     "return function(fn,first,last) local sum=.5; for i=first,last do "
       "sum=sum+fn(i,i+1,i+2) end; return sum end",
     3, BC_ADDVV, 0, 2, {{1073741823,1073741824,0},{1073741822,1073741824,1}},
     {0,1}, 2, 2,1, -1},
    {"dead-local", "local dead=b*2; dead=a; return a*3",
     "return function(fn,first,last) local sum=.5; for i=first,last do "
       "sum=sum+fn(i,1) end; return sum end",
     2, BC_MULVN, 1, 2, {{715827882,715827883,0},{715827881,715827883,1}},
     {2,0}, 2, 0,0, -1},
    {"later-param", "local x=a*3; return x+b",
     "return function(fn,first,last) local sum=.5; for i=first,last do "
       "sum=sum+fn(i,1) end; return sum end",
     2, BC_MULVN, 0, 2, {{715827882,715827883,0},{715827881,715827883,1}},
     {0,0}, 1, 0,0, -1},
    {"cset-old", "local x=a; x=x*3; return x+b",
     "return function(fn,first,last) local sum=.5; for i=first,last do "
       "sum=sum+fn(i,1) end; return sum end",
     2, BC_MULVN, 0, 2, {{715827882,715827883,0},{715827881,715827883,1}},
     {0,0}, 1, 0,0, 2}
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  assert(LJ_FR2 == 1);
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "assert(select(2,s:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5)))");
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    const SparseCase *c = &cases[i];
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
    admission(T,c);
    printf("Lua sparse exits %s: ranges=%u native trace reused\n",
           c->name,c->nranges);
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  assert(native_runs == 36 && child_exits == 12 &&
         same_trace_reuses == 24 && gc_reuses == 6 && natural_holes == 16 &&
         callmap_only == 1 && executable_locations == 3 && boundary_bad == 120);
  assert(positive_omissions > 0 && semantic_bad > 0 && postra_bad > 0 &&
         negative_live[0] > 0 && negative_live[1] > 0 &&
         negative_live[2] > 0 && alias_bad > 0 &&
         omission_by_case[0] > 0 && omission_by_case[1] > 0 &&
         omission_by_case[2] > 0 && omission_by_case[3] > 0);
  printf("Lua sparse exits OK: native=%u child=%u reuse=%u gc=%u "
         "natural-holes=%u dead-omissions=%u semantic-negative=%u postRA-negative=%u aliases=%u "
         "callmap-only=%u executable-locations=%u boundaries=%u\n",
         native_runs,child_exits,same_trace_reuses,gc_reuses,
         natural_holes,positive_omissions,semantic_bad,postra_bad,alias_bad,
         callmap_only,executable_locations,boundary_bad);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-sparse-exits SKIP"); return 0; }
#endif
