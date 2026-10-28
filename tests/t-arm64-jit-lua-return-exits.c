/* Native terminal Lua return restoration and independent admission checks. */
#include <stdio.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"

typedef struct ReturnCase {
  const char *name, *params, *body, *args;
  unsigned captured;
} ReturnCase;

static const char *active_name;
static unsigned cases_run, native_runs, terminal_exits, same_trace_reuses;
static unsigned gc_reuses, primitive_mismatches;

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

static const BCIns *only_call(GCproto *pt)
{
  const BCIns *call = NULL, *bc = proto_bc(pt);
  for (MSize p = 1; p < pt->sizebc; p++) {
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[p])) == BC_CALL) {
      assert(call == NULL); call = &bc[p];
    }
  }
  assert(call); return call;
}

static uint64_t snapshot_footer(const GCtrace *T, SnapNo s)
{
  const SnapShot *snap = &T->snap[s];
  uint64_t footer;
  memcpy(&footer, &T->snapmap[snap->mapofs+snap->nent], sizeof(footer));
  return footer;
}

static GCtrace *root_trace(lua_State *L)
{
  GCproto *pt = funcproto(global_function(L, "leaf_run"));
  for (MSize p = 1; p < pt->sizebc; p++) {
    BCIns bc = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[p]);
    if (bc_op(bc) == BC_JFORL) {
      TraceNo no = bc_d(bc);
      GCtrace *T = traceref_safe(L2J(L), no);
      if (T && trace_runnable_acq(T, no) && !T->root &&
          trace_startpt_acq(T) == pt && T->linktype == LJ_TRLINK_LOOP &&
          T->link == no) return T;
    }
  }
  return NULL;
}

/* Keep the oracle result on the Lua stack while the candidate runs, including
** GC objects. A C-local TValue would not itself be an enumerated root. */
static void invoke(lua_State *L, const char *run, const char *leaf,
                   lua_Integer first, lua_Integer last)
{
  void *cframe = L->cframe;
  ptrdiff_t base = L->base-tvref(L->stack), top = L->top-tvref(L->stack);
  lua_getglobal(L, run); lua_getglobal(L, leaf);
  lua_pushinteger(L, first); lua_pushinteger(L, last);
  lua_getglobal(L, "arg_payload");
  ljt_lua_pcall(L, 4, 1, active_name);
  assert(L->base-tvref(L->stack) == base && L->top-tvref(L->stack) == top+1 &&
         L->cframe == cframe && lj_tg_load_jit_base(L2TG(L)) == NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
}

typedef struct OldTagChange {
  TGState *tg;
  GCfunc *leaf;
  BCReg local;
  IRType type;
  uint32_t changed;
} OldTagChange;

/* A controlled old-tag mismatch uses the existing one-shot entry pause.
** The owner is stopped after admission and before generated code runs.
** Publish only a primitive replacement, with no new GC pointer or code write. */
static void *change_old_tag(void *opaque)
{
  OldTagChange *change = (OldTagChange *)opaque;
  TValue *base, fn, old, replacement;
  unsigned attempt;
  for (attempt = 0; attempt < 10000000u; attempt++) {
    if (lj_trace_test_root_entry_paused() ==
        LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(attempt < 10000000u);
  base = lj_tg_load_jit_base(change->tg);
  assert(base);
  fn.u64 = tv_rawload_acq(&base[0]);
  old.u64 = tv_rawload_acq(&base[change->local]);
  assert(tvisfunc(&fn) && funcV(&fn) == change->leaf);
  assert(itype2irt(&old) == change->type);
  if (tvisnil(&old)) setboolV(&replacement, 0);
  else setnilV(&replacement);
  assert(itype2irt(&replacement) != change->type);
  tv_rawstore_rel(&base[change->local], replacement.u64);
  la_store32_rel(&change->changed, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static SnapNo terminal_snapshot(GCtrace *T, GCproto *leaf)
{
  const BCIns *ret = proto_bc(leaf)+leaf->sizebc-1u;
  for (SnapNo s = 0; s < T->nsnap; s++) {
    uint64_t footer = snapshot_footer(T, s);
    IRRef r = T->snap[s].ref;
    if ((uint8_t)footer && (const BCIns *)(uintptr_t)(footer>>8) == ret &&
        T->ir[r].o == IR_SLOAD && irt_isguard(T->ir[r].t)) return s;
  }
  fprintf(stderr, "%s has no terminal old-tag guard\n", active_name);
  abort();
}

static void assert_terminal(lua_State *L, GCtrace *T, SnapNo expected)
{
  GCfunc *fn = global_function(L, "leaf_candidate");
  GCproto *leaf = funcproto(fn), *root = trace_startpt_acq(T);
  const BCIns *call = only_call(root), *ret = proto_bc(leaf)+leaf->sizebc-1u;
  ExitNo exit = lj_trace_test_first_exitno();
  const SnapShot *snap;
  uint64_t footer;
  MSize frame, base;
  unsigned found_fn = 0, found_link = 0, found_value = 0;
  assert(exit == expected && exit < T->nsnap);
  snap = &T->snap[exit]; footer = snapshot_footer(T, exit);
  base = (uint8_t)footer+2u; frame = base-1u;
  assert(bc_op(*ret) == BC_RET1 && bc_d(*ret) == 2 &&
         (const BCIns *)(uintptr_t)(footer>>8) == ret &&
         base == 2u+2u*LJ_FR2+bc_a(*call));
  for (MSize i = 0; i < snap->nent; i++) {
    SnapEntry entry = T->snapmap[snap->mapofs+i];
    IRRef r = snap_ref(entry);
    if (snap_slot(entry) == frame) {
      assert((entry & 0x00ff0000u) == SNAP_FRAME &&
             T->ir[r].o == IR_KNUM &&
             T->ir[r+1u].tv.u64 == (uint64_t)(uintptr_t)(call+1));
      found_link++;
    }
    if (snap_slot(entry) == base-2u) {
      assert(!(entry & 0x00ff0000u) && T->ir[r].o == IR_KGC &&
             ir_kgc_acq(&T->ir[r]) == obj2gco(fn));
      found_fn++;
    }
    if (snap_slot(entry) == base+bc_a(*ret)) {
      assert(!(entry & 0x00ff0000u) &&
             irt_type(T->ir[r].t) != IRT_P32 &&
             irt_type(T->ir[r].t) != IRT_PGC);
      found_value++;
    }
  }
  assert(found_fn == 1 && found_link == 1 && found_value == 1);
}

static void assert_branch_exit(lua_State *L, GCtrace *T)
{
  GCfunc *fn = global_function(L, "leaf_candidate");
  GCproto *leaf = funcproto(fn);
  const BCIns *bc = proto_bc(leaf), *branch = NULL;
  ExitNo exit = lj_trace_test_first_exitno();
  uint64_t footer;
  BCIns cmp, jump;
  MSize compare = UINT32_MAX, cold;
  assert(exit < T->nsnap);
  for (MSize p = 1; p+1u < leaf->sizebc; p++) {
    cmp = (BCIns)la_load32_acq((const uint32_t *)&bc[p]);
    if (bc_op(cmp) >= BC_ISLT && bc_op(cmp) <= BC_ISGT) {
      compare = p; branch = &bc[p]; break;
    }
  }
  assert(branch && compare+1u < leaf->sizebc);
  jump = (BCIns)la_load32_acq((const uint32_t *)&bc[compare+1u]);
  assert(bc_op(jump) == BC_JMP);
  /* Bind the exact observed guard polarity to its source edge. Numeric
  ** guards retain Lua's unordered relation bits when the recorder flips an
  ** observed-false predicate; canonicalization may also swap operands. */
  {
    IRIns guard = T->ir[T->snap[exit].ref];
    BCOp sourceop = bc_op(*branch);
    IROp op = (IROp)((int)sourceop-(int)BC_ISLT+(int)IR_LT);
    IROp candidate[2];
    unsigned i, found = 0;
    int cond = 0;
    if (irt_type(guard.t) == IRT_NUM && (op & 1)) op = (IROp)(op^4);
    candidate[0] = op;
    candidate[1] = (IROp)(op^(irt_type(guard.t) == IRT_NUM ? 5 : 1));
    for (i = 0; i < 2; i++)
      if (guard.o == candidate[i] || guard.o == (IROp)(candidate[i]^3)) {
        cond = ((int)sourceop ^ (int)candidate[i]) & 1;
        found++;
      }
    assert(found == 1);
    cold = cond ? (MSize)((ptrdiff_t)(compare+2u)+bc_j(jump)) : compare+2u;
  }
  footer = snapshot_footer(T, exit);
  assert((uint8_t)footer &&
         (const BCIns *)(uintptr_t)(footer>>8) == &bc[cold] &&
         cold < leaf->sizebc-1u);
  assert(T->ir[T->snap[exit].ref].o >= IR_LT &&
         T->ir[T->snap[exit].ref].o <= IR_UGT &&
         irt_isguard(T->ir[T->snap[exit].ref].t));
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno) &&
         !trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void checked(lua_State *L, GCtrace *T, int force,
                    lua_Integer first, lua_Integer last)
{
  pthread_t worker;
  OldTagChange change = {0};
  SnapNo terminal = 0;
  invoke(L, "leaf_oracle_run", "leaf_oracle", first, last);
  if (T) {
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    if (force) {
      GCfunc *fn = global_function(L, "leaf_candidate");
      const BCIns *call = only_call(trace_startpt_acq(T));
      terminal = terminal_snapshot(T, funcproto(fn));
      IRIns check = T->ir[T->snap[terminal].ref];
      assert(bc_op(call[1]) == BC_CSET && bc_d(call[1]) == bc_a(*call) &&
             bc_a(call[1]) < bc_a(*call) &&
             check.op1 == 1u+LJ_FR2+bc_a(call[1]) &&
             check.op2 == IRSLOAD_TYPECHECK);
      change.tg = L2TG(L); change.leaf = fn; change.local = bc_a(call[1]);
      change.type = irt_type(check.t);
      lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
      assert(pthread_create(&worker, NULL, change_old_tag, &change) == 0);
    }
  }
  invoke(L, "leaf_run", "leaf_candidate", first, last);
  assert(lua_rawequal(L, -1, -2)); lua_pop(L, 2);
  if (!T) return;
  if (force) {
    assert(pthread_join(worker, NULL) == 0 && la_load32_acq(&change.changed));
    assert(!lj_trace_test_root_entry_paused());
    assert_terminal(L, T, terminal);
    primitive_mismatches++; terminal_exits++;
  } else same_trace_reuses++;
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno) &&
         lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno &&
         !trace_nchild_acq(T) && !trace_nextside_acq(T));
  native_runs++;
}

static void install_case(lua_State *L, const ReturnCase *shape)
{
  char source[4096], runner[1024];
  active_name = shape->name;
  snprintf(runner, sizeof(runner),
    "return function(fn,first,last,payload) local value; %s "
    "for i=first,last do value=fn(%s) end; return value end",
    shape->captured ? "local capture=function() return value end;" : "",
    shape->args);
  snprintf(source, sizeof(source),
    "leaf_candidate=assert(loadstring('return function(%s) %s end',"
      "'@return-candidate-%s'))(); "
    "leaf_oracle=assert(loadstring('return function(%s) %s end',"
      "'@return-oracle-%s'))(); "
    "leaf_run=assert(loadstring('%s','@return-run-%s'))(); "
    "leaf_oracle_run=assert(loadstring('%s','@return-oracle-run-%s'))(); "
    "jit.off(leaf_oracle,true); jit.off(leaf_oracle_run,true); "
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64')",
    shape->params,shape->body,shape->name,shape->params,shape->body,shape->name,
    runner,shape->name,runner,shape->name);
  ljt_lua_dostring(L, source);
}

typedef struct ReturnFixture {
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
  BCReg childbase, result_slot;
} ReturnFixture;

static unsigned ret_semantic_bad, ret_postra_bad, ret_alias_bad;
static unsigned ret_terminal_maps, ret_next_map_bad, ret_callmap_guards;
/* Every semantic mutation must run in both passes; allocation conflicts
** and the guarded loop-increment mutation belong to the allocated pass. */
static unsigned ret_mutation_count[26][2];
static unsigned branch_mutation_count[6][2];

enum {
  RET_NEG_SWAP = 0,
  RET_NEG_OMIT,
  RET_NEG_NORESTORE,
  RET_NEG_A,
  RET_NEG_D,
  RET_NEG_PC,
  RET_NEG_CSET_SRC,
  RET_NEG_CSET_DST,
  RET_NEG_CALL_A,
  RET_NEG_SLOAD_SLOT,
  RET_NEG_SLOAD_MODE,
  RET_NEG_SLOAD_TYPE,
  RET_NEG_NSLOTS,
  RET_NEG_TOPSLOT,
  RET_NEG_NEXT_PC,
  RET_NEG_DEAD_NONNUM,
  RET_NEG_DCE_GUARD,
  RET_NEG_NEXT_OMIT,
  RET_NEG_UNKNOWN_RETURN,
  RET_NEG_KPRI_SWAP,
  RET_NEG_KPRI_RANGE,
  RET_NEG_KPRI_ARITH,
  RET_NEG_ALIAS,
  RET_NEG_CALLMAP_GUARD,
  RET_NEG_BRANCH_TARGET,
  RET_NEG_BRANCH_OPERAND,
  RET_NEG_BRANCH_POLARITY,
  RET_NEG_BRANCH_FUTURE_MAPOFS,
  RET_NEG_BRANCH_FUTURE_NENT,
  RET_NEG_BRANCH_OMIT_LIVE
};

static GCproto *return_copy_proto(const GCproto *pt)
{
  GCproto *copy = malloc(pt->sizept);
  assert(copy != NULL);
  memcpy(copy, pt, pt->sizept);
  return copy;
}

static MSize return_map_entry(const SnapEntry *map, MSize ofs, MSize nent,
                              BCReg slot)
{
  for (MSize i = 0; i < nent; i++)
    if (snap_slot(map[ofs+i]) == slot) return ofs+i;
  return UINT32_MAX;
}

static void return_remove_entry(ReturnFixture *f, SnapNo sn, MSize entry)
{
  SnapShot *s = &f->snap[sn];
  assert(entry >= s->mapofs && entry < s->mapofs+s->nent);
  memmove(&f->map[entry], &f->map[entry+1u],
          (f->copy.nsnapmap-entry-1u)*sizeof(SnapEntry));
  s->nent--;
  for (SnapNo i = sn+1u; i < f->copy.nsnap; i++) f->snap[i].mapofs--;
  f->copy.nsnapmap--;
  f->view.nsnapmap--;
}

static RegSP return_value_location(const ReturnFixture *f, IRRef ref,
                                   SnapNo snapno)
{
  RegSP rs = f->ir[ref].prev;
  for (IRRef r = f->live->nins; r-- > f->n;)
    if (f->ir[r].o == IR_RENAME && f->ir[r].op1 == ref &&
        f->ir[r].op2 <= snapno) rs = f->ir[r].prev;
  return rs;
}

static void return_reset_fixture(ReturnFixture *f)
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
  memcpy(f->ir+T->nk, T->ir+T->nk,
         (T->nins-T->nk)*sizeof(IRIns));
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
    uint64_t footer;
    MSize pos;
    memcpy(&footer, &T->snapmap[snap->mapofs+snap->nent], sizeof(footer));
    if ((uint8_t)footer) {
      pos = proto_bcpos(pt, (const BCIns *)(uintptr_t)(footer>>8));
      assert(pos < pt->sizebc);
      footer = (u64ptr(proto_bc(f->leafpt)+pos)<<8)|(uint8_t)footer;
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
      pos = proto_bcpos(root, (const BCIns *)(uintptr_t)(footer>>8));
      assert(pos < root->sizebc);
      footer = u64ptr(proto_bc(f->root)+pos)<<8;
    }
    memcpy(&f->map[snap->mapofs+snap->nent], &footer, sizeof(footer));
  }
  memset(f->J, 0, sizeof(*f->J));
  f->J->pt = f->root;
  f->J->startpc = trace_startpc_acq(&f->copy);
  f->J->baseslot = 1u+LJ_FR2;
  for (IRRef r = REF_FIRST; r < f->n; r++)
    if (f->ir[r].o == IR_LOOP) f->loopref = f->J->loopref = r;
  f->view = (LJArm64PostRAView){0};
  f->view.ir = f->ir; f->view.owner = &f->copy;
  f->view.owner_private = 1;
  f->view.snap = f->snap; f->view.snapmap = f->map;
  f->view.proto = f->root; f->view.proto_bc = proto_bc(f->root);
  f->view.startpc = trace_startpc_acq(&f->copy);
  f->view.nins = T->nins; f->view.nk = T->nk;
  f->view.nsnap = T->nsnap; f->view.nsnapmap = T->nsnapmap;
  f->view.spadjust = T->spadjust; f->view.proto_sizebc = f->root->sizebc;
  f->view.proto_numparams = f->root->numparams;
  f->view.root_topslot = T->topslot; f->view.startins = T->startins;
}

static int return_admit(ReturnFixture *f, int postra)
{
  if (postra) return lj_asm_arm64_postra_admit(&f->view, NULL);
  f->copy.nins = f->n;
  return lj_asm_arm64_ir_admit(f->J, &f->copy, NULL);
}

static void return_expect_reject(ReturnFixture *f, unsigned id, int postra)
{
  if (return_admit(f, postra)) {
    fprintf(stderr, "return %s mutation=%u snapshot=%u accepted %s\n",
            active_name, id, f->child, postra ? "postRA" : "semantic");
    abort();
  }
  if (id >= RET_NEG_BRANCH_TARGET)
    branch_mutation_count[id-RET_NEG_BRANCH_TARGET][postra]++;
  else
    ret_mutation_count[id][postra]++;
  if (postra) ret_postra_bad++; else ret_semantic_bad++;
}

static void return_test_negative(ReturnFixture *f, SnapNo s, unsigned id,
                                int postra, unsigned which)
{
  SnapShot *snap;
  uint64_t footer;
  MSize entry, pos;
  BCIns ret;
  return_reset_fixture(f);
  f->child = s;
  assert(return_admit(f, 0) && return_admit(f, 1));
  if (which == RET_NEG_BRANCH_POLARITY) {
    if (postra) return;
    for (IRRef r = REF_FIRST; r < f->n; r++) {
      IRIns *guard = &f->ir[r];
      if (!(guard->t.irt & IRT_GUARD) ||
          (guard->o != IR_LT && guard->o != IR_GE &&
           guard->o != IR_LE && guard->o != IR_GT &&
           guard->o != IR_ULT && guard->o != IR_UGE &&
           guard->o != IR_ULE && guard->o != IR_UGT)) continue;
      guard->o ^= 1;
      if (!return_admit(f, 0) && !return_admit(f, 1)) {
        return_expect_reject(f, id, 0);
        return_expect_reject(f, id, 1);
        return;
      }
      return_reset_fixture(f); f->child = s;
    }
    return;
  }
  if (which == RET_NEG_BRANCH_OMIT_LIVE) {
    MSize cmp = UINT32_MAX, target;
    BCIns jump;
    if (postra || strcmp(active_name, "comparison-arm-liveness")) return;
    for (MSize p = 1; p+2u < f->leafpt->sizebc; p++)
      if (bc_op(proto_bc(f->leafpt)[p]) >= BC_ISLT &&
          bc_op(proto_bc(f->leafpt)[p]) <= BC_ISGT) { cmp = p; break; }
    if (cmp == UINT32_MAX) return;
    jump = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(f->leafpt)[cmp+1u]);
    if (bc_op(jump) != BC_JMP) return;
    target = (MSize)((ptrdiff_t)(cmp+2u)+bc_j(jump));
    for (SnapNo q = 0; q < f->view.nsnap; q++) {
      uint64_t edge;
      ptrdiff_t pos;
      MSize mapped;
      BCReg childbase, local = 0;
      return_reset_fixture(f); f->child = s;
      assert(return_admit(f, 0) && return_admit(f, 1));
      edge = snapshot_footer(&f->copy, q);
      if (!(uint8_t)edge) continue;
      pos = (ptrdiff_t)((const BCIns *)(uintptr_t)(edge>>8)-proto_bc(f->leafpt));
      if (pos != (ptrdiff_t)(cmp+2u) && pos != (ptrdiff_t)target) continue;
      childbase = (BCReg)((uint8_t)edge+2u);
      mapped = return_map_entry(f->map, f->snap[q].mapofs,
                                f->snap[q].nent,
                                (BCReg)(childbase+local));
      if (mapped == UINT32_MAX) continue;
      return_remove_entry(f, q, mapped);
      if (!return_admit(f, 0) && !return_admit(f, 1)) {
        return_expect_reject(f, id, 0);
        return_expect_reject(f, id, 1);
        return;
      }
    }
    return;
  }
  snap = &f->snap[s];
  memcpy(&footer, &f->map[snap->mapofs+snap->nent], sizeof(footer));
  f->childbase = (BCReg)((uint8_t)footer+2u);
  pos = proto_bcpos(f->leafpt, (const BCIns *)(uintptr_t)(footer>>8));
  assert(pos+1u == f->leafpt->sizebc);
  ret = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(f->leafpt)[pos]);
  assert(bc_op(ret) == BC_RET1 && bc_d(ret) == 2u);
  f->result_slot = (BCReg)(f->childbase+bc_a(ret));
  entry = return_map_entry(f->map, snap->mapofs, snap->nent, f->result_slot);
  assert(entry != UINT32_MAX);

  switch (which) {
  case RET_NEG_SWAP: {
    IRRef value = snap_ref(f->map[entry]);
    MSize other = UINT32_MAX;
    if (value < REF_FIRST) return;
    for (MSize i = 0; i < snap->nent; i++) {
      IRRef r = snap_ref(f->map[snap->mapofs+i]);
      if (i != entry-snap->mapofs && r >= REF_FIRST &&
          r < snap_ref_acq(snap) &&
          irt_type(f->ir[r].t) == irt_type(f->ir[value].t) && r != value) {
        other = snap->mapofs+i; break;
      }
    }
    if (other == UINT32_MAX) return;
    f->map[entry] = SNAP(f->result_slot,
        f->map[entry]&0x00ff0000u, snap_ref(f->map[other]));
    break;
  }
  case RET_NEG_OMIT:
    return_remove_entry(f, s, entry);
    break;
  case RET_NEG_NORESTORE:
    f->map[entry] |= SNAP_NORESTORE;
    break;
  case RET_NEG_A: {
    BCReg wrong = f->leafpt->framesize;
    for (BCReg slot = 0; slot < f->leafpt->framesize; slot++) {
      MSize mapped = return_map_entry(f->map, snap->mapofs, snap->nent,
                                      (BCReg)(f->childbase+slot));
      if (slot != bc_a(ret) && mapped != UINT32_MAX &&
          snap_ref(f->map[mapped]) != snap_ref(f->map[entry])) {
        wrong = slot; break;
      }
    }
    /* Prefer another defined, distinct child local; otherwise test the bound. */
    setbc_a(&proto_bc(f->leafpt)[pos], wrong);
    break;
  }
  case RET_NEG_D:
    setbc_d(&proto_bc(f->leafpt)[pos], 1u);
    break;
  case RET_NEG_PC:
    footer = (u64ptr(proto_bc(f->leafpt)+pos-1u)<<8)|(uint8_t)footer;
    memcpy(&f->map[snap->mapofs+snap->nent], &footer, sizeof(footer));
    break;
  case RET_NEG_CSET_SRC:
  case RET_NEG_CSET_DST:
  case RET_NEG_CALL_A: {
    const BCIns *call = only_call(f->root);
    MSize callpos = proto_bcpos(f->root, call), csetpos = f->root->sizebc;
    BCReg call_a = bc_a(*call), dst = 0;
    for (MSize p = callpos+1u; p < f->root->sizebc; p++) {
      BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(f->root)[p]);
      if (bc_op(ins) == BC_CSET && bc_d(ins) == call_a) {
        csetpos = p; dst = bc_a(ins); break;
      }
      if (bc_op(ins) == BC_FORL || bc_isret_or_tail(bc_op(ins))) break;
    }
    assert(csetpos < f->root->sizebc && dst+1u < f->root->framesize);
    if (which == RET_NEG_CSET_SRC)
      setbc_d(&proto_bc(f->root)[csetpos], call_a+1u);
    else if (which == RET_NEG_CSET_DST)
      setbc_a(&proto_bc(f->root)[csetpos], dst+1u);
    else
      setbc_a((BCIns *)(uintptr_t)call, call_a+1u);
    break;
  }
  case RET_NEG_SLOAD_SLOT:
  case RET_NEG_SLOAD_MODE:
  case RET_NEG_SLOAD_TYPE: {
    /* The terminal RET1 map's ref is the caller's old-destination guard, not
    ** the child's returned operand (which can be a constant or REF_NIL). */
    IRRef source = snap_ref_acq(snap);
    IRIns *load;
    assert(source >= REF_FIRST && source < f->n && f->ir[source].o == IR_SLOAD);
    load = &f->ir[source];
    if (which == RET_NEG_SLOAD_SLOT) {
      assert(load->op1+1u < f->root->framesize);
      load->op1 = 1u+LJ_FR2;
    } else if (which == RET_NEG_SLOAD_MODE) {
      load->op2 = IRSLOAD_READONLY;
    } else {
      load->t.irt = (IRType)((load->t.irt & ~IRT_TYPE) | IRT_PGC);
    }
    break;
  }
  case RET_NEG_NSLOTS:
    assert(snap->nslots > f->childbase+1u);
    snap->nslots--;
    break;
  case RET_NEG_TOPSLOT:
    assert(snap->topslot > 0);
    snap->topslot--;
    break;
  case RET_NEG_NEXT_PC: {
    SnapNo next = s+1u;
    uint64_t next_footer;
    assert(next < f->copy.nsnap);
    memcpy(&next_footer, &f->map[f->snap[next].mapofs+f->snap[next].nent],
           sizeof(next_footer));
    next_footer = (u64ptr(proto_bc(f->root)+f->root->sizebc)<<8);
    memcpy(&f->map[f->snap[next].mapofs+f->snap[next].nent], &next_footer,
           sizeof(next_footer));
    break;
  }
  case RET_NEG_DEAD_NONNUM: {
    /* Change the caller's first actual to an existing table SLOAD. The leaf's
    ** first formal is dead at RET1 but is read by its earlier numeric op. */
    const BCIns *call = only_call(f->root);
    MSize caller = s, replacement = UINT32_MAX, callpos;
    int found_caller = 0;
    BCReg argslot;
    if (strncmp(active_name, "dead-prefix-", 12) != 0) return;
    callpos = proto_bcpos(f->root, call);
    while (caller) {
      uint64_t cf;
      --caller;
      memcpy(&cf, &f->map[f->snap[caller].mapofs+f->snap[caller].nent],
             sizeof(cf));
      if (!(uint8_t)cf &&
          (const BCIns *)(uintptr_t)(cf>>8) ==
             proto_bc(f->root)+callpos) {
        found_caller = 1;
        break;
      }
    }
    if (!found_caller) return;
    for (IRRef r = REF_FIRST; r < f->n; r++)
      if (f->ir[r].o == IR_SLOAD && irt_type(f->ir[r].t) == IRT_TAB) {
        replacement = r; break;
      }
    if (replacement == UINT32_MAX) return;
    argslot = (BCReg)(1u+LJ_FR2+bc_a(*call)+1u+LJ_FR2);
    entry = return_map_entry(f->map, f->snap[caller].mapofs,
                             f->snap[caller].nent, argslot);
    if (entry == UINT32_MAX) return;
    f->map[entry] = SNAP(argslot,
        f->map[entry]&0x00ff0000u, (IRRef)replacement);
    break;
  }
  case RET_NEG_NEXT_OMIT: {
    const BCIns *call = only_call(f->root);
    IRIns guard = f->ir[snap->ref];
    BCReg target = (BCReg)(1u+LJ_FR2+
      (irt_type(guard.t) == IRT_P32 ? bc_a(*call) : bc_a(call[1])));
    SnapShot *next = &f->snap[s+1u];
    entry = return_map_entry(f->map, next->mapofs, next->nent, target);
    assert(entry != UINT32_MAX);
    return_remove_entry(f, s+1u, entry);
    break;
  }
  case RET_NEG_UNKNOWN_RETURN: {
    /* A discarded arithmetic result has no SSA identity at RET1. Replacing
    ** the source with that local must not turn an unrelated map value into it. */
    BCReg dead = 0;
    int found = 0;
    if (strncmp(active_name, "comparison-", 11) == 0) {
      IRRef original = snap_ref(f->map[entry]);
      BCReg alternate = f->leafpt->framesize;
      for (BCReg slot = 0; slot < f->leafpt->framesize; slot++) {
        MSize mapped = return_map_entry(f->map, snap->mapofs, snap->nent,
                                        (BCReg)(f->childbase+slot));
        if (mapped != UINT32_MAX &&
            snap_ref(f->map[mapped]) != original) {
          alternate = slot; break;
        }
      }
      if (alternate >= f->leafpt->framesize) return;
      setbc_a(&proto_bc(f->leafpt)[pos], alternate);
      break;
    }
    if (strncmp(active_name, "dead-prefix-", 12) != 0) return;
    for (MSize p = 1; p < pos; p++) {
      BCIns ins = proto_bc(f->leafpt)[p];
      if (bc_op(ins) == BC_MULVN) { dead = bc_a(ins); found = 1; break; }
    }
    assert(found);
    setbc_a(&proto_bc(f->leafpt)[pos], dead);
    break;
  }
  case RET_NEG_BRANCH_TARGET:
  case RET_NEG_BRANCH_OPERAND: {
    MSize cmp = UINT32_MAX;
    if (strncmp(active_name, "comparison-", 11) != 0) return;
    for (MSize p = 1; p+2u < pos; p++)
      if (bc_op(proto_bc(f->leafpt)[p]) >= BC_ISLT &&
          bc_op(proto_bc(f->leafpt)[p]) <= BC_ISGT) { cmp = p; break; }
    if (cmp == UINT32_MAX) return;
    if (which == RET_NEG_BRANCH_OPERAND) {
      BCIns ins = (BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(f->leafpt)[cmp]);
      setbc_d(&proto_bc(f->leafpt)[cmp], bc_a(ins));
    } else {
      BCIns jump = (BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(f->leafpt)[cmp+1u]);
      assert(bc_op(jump) == BC_JMP &&
             (MSize)((ptrdiff_t)(cmp+2u)+bc_j(jump)+1) < pos);
      setbc_j(&proto_bc(f->leafpt)[cmp+1u], bc_j(jump)+1);
    }
    break;
  }
  case RET_NEG_BRANCH_POLARITY: return;
  case RET_NEG_BRANCH_FUTURE_MAPOFS:
  case RET_NEG_BRANCH_FUTURE_NENT: {
    SnapNo q = (SnapNo)(f->view.nsnap-1u);
    if (which == RET_NEG_BRANCH_FUTURE_MAPOFS)
      f->snap[q].mapofs = f->view.nsnapmap;
    else
      f->snap[q].nent = f->view.nsnapmap-f->snap[q].mapofs;
    break;
  }
  case RET_NEG_KPRI_RANGE: {
    MSize p;
    for (p = 1; p < pos; p++) {
      BCIns ins = (BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(f->leafpt)[p]);
      if (bc_op(ins) == BC_KPRI) break;
    }
    if (p == pos) return;
    setbc_d(&proto_bc(f->leafpt)[p], 3u);
    break;
  }
  case RET_NEG_KPRI_SWAP: {
    MSize p;
    for (p = 1; p < pos; p++)
      if (bc_op(proto_bc(f->leafpt)[p]) == BC_KPRI) break;
    if (p == pos) return;
    setbc_d(&proto_bc(f->leafpt)[p], (bc_d(proto_bc(f->leafpt)[p])+1u)%3u);
    break;
  }
  case RET_NEG_KPRI_ARITH: {
    MSize p, q;
    BCReg value;
    for (p = 1; p < pos; p++)
      if (bc_op(proto_bc(f->leafpt)[p]) == BC_KPRI) break;
    if (p == pos) return;
    value = bc_a(proto_bc(f->leafpt)[p]);
    for (q = p+1u; q < pos; q++)
      if (bc_op(proto_bc(f->leafpt)[q]) == BC_CSET) break;
    if (q == pos) return;
    setbc_op(&proto_bc(f->leafpt)[q], BC_ADDVV);
    setbc_a(&proto_bc(f->leafpt)[q], value);
    setbc_b(&proto_bc(f->leafpt)[q], value);
    setbc_c(&proto_bc(f->leafpt)[q], value);
    break;
  }
  case RET_NEG_DCE_GUARD: {
    IRRef nop = 0, lhs = 0, rhs = 0;
    for (IRRef r = REF_FIRST; r < f->n && !nop; r++) {
      if (f->ir[r].o != IR_NOP) continue;
      for (IRRef a = REF_FIRST; a < r && !lhs; a++) {
        if (f->ir[a].o != IR_SLOAD || irt_type(f->ir[a].t) != IRT_INT) continue;
        for (IRRef b = a+1u; b < r; b++)
          if (f->ir[b].o == IR_SLOAD && irt_type(f->ir[b].t) == IRT_INT) {
            lhs=a; rhs=b; break;
          }
      }
      if (lhs && rhs) nop = r;
    }
    if (!nop) return;
    f->ir[nop].o = IR_EQ;
    f->ir[nop].t.irt = IRT_INT|IRT_GUARD;
    f->ir[nop].op1 = lhs; f->ir[nop].op2 = rhs;
    break;
  }
  case RET_NEG_ALIAS: {
    IRRef value = snap_ref(f->map[entry]);
    RegSP from, to;
    MSize other = UINT32_MAX;
    if (!postra || value < REF_FIRST) return;
    from = return_value_location(f, value, s);
    if (!ra_hasreg(regsp_reg(from)) || ra_hasspill(regsp_spill(from))) return;
    for (MSize i = 0; i < snap->nent; i++) {
      IRRef r = snap_ref(f->map[snap->mapofs+i]);
      RegSP candidate;
      if (i == entry-snap->mapofs || r < REF_FIRST || r == value) continue;
      if (irt_type(f->ir[r].t) != irt_type(f->ir[value].t)) continue;
      candidate = return_value_location(f, r, s);
      if (!ra_hasreg(regsp_reg(candidate)) ||
          ra_hasspill(regsp_spill(candidate)) ||
          regsp_reg(candidate) == regsp_reg(from)) continue;
      other = i; to = candidate; break;
    }
    if (other == UINT32_MAX) return;
    f->ir[value].r = regsp_reg(to); f->ir[value].s = SPS_NONE;
    for (IRRef ren = f->n; ren < f->live->nins; ren++)
      if (f->ir[ren].o == IR_RENAME && f->ir[ren].op1 == value &&
          f->ir[ren].op2 <= s) {
        f->ir[ren].r = regsp_reg(to); f->ir[ren].s = SPS_NONE;
      }
    break;
  }
  default: assert(0);
  }
  return_expect_reject(f, id, postra);
  if (which == RET_NEG_ALIAS) ret_alias_bad++;
}

static void admission(GCtrace *T)
{
  ReturnFixture f = {0};
  GCfunc *leaf = NULL;
  GCproto *pt;
  f.live = T; f.n = T->nins;
  if (T->ir[f.n-1u].o == IR_NOP) f.n--;
  else while (f.n > T->nk && T->ir[f.n-1u].o == IR_RENAME) f.n--;
  for (SnapNo s = 0; s < T->nsnap && !leaf; s++) {
    uint64_t footer = snapshot_footer(T, s);
    MSize low, base, pos;
    const BCIns *pc;
    if (!(uint8_t)footer) continue;
    pc = (const BCIns *)(uintptr_t)(footer>>8);
    /* A RET1 terminal child map identifies its exact callee in the frame. */
    low = (uint8_t)footer;
    base = low+2u;
    /* The leaf prototype is obtained from the frame's constant before cloning. */
    for (MSize i = 0; i < T->snap[s].nent; i++) {
      SnapEntry e = T->snapmap[T->snap[s].mapofs+i];
      IRRef r = snap_ref(e);
      if (snap_slot(e) == base-2u && r < T->nins && T->ir[r].o == IR_KGC) {
        GCfunc *candidate = gco2func(ir_kgc_acq(&T->ir[r]));
        if (isluafunc(candidate)) { f.kfunc = r; leaf = candidate; break; }
      }
    }
    if (leaf) {
      pt = funcproto(leaf);
      pos = proto_bcpos(pt, pc);
      if (pos+1u != pt->sizebc ||
          bc_op((BCIns)la_load32_acq((const uint32_t *)pc)) != BC_RET1) {
        leaf = NULL; f.kfunc = 0;
      }
    }
  }
  assert(f.kfunc && leaf);
  pt = funcproto(leaf);
  f.root = return_copy_proto(trace_startpt_acq(T));
  f.leafpt = return_copy_proto(pt);
  f.leaf = calloc(1, sizeof(*f.leaf)); f.J = calloc(1, sizeof(*f.J));
  f.ir = calloc(T->nins+1u, sizeof(IRIns));
  f.snap = malloc(T->nsnap*sizeof(SnapShot));
  f.map = malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf && f.J && f.ir && f.snap && f.map);

  for (SnapNo s = 0; s < T->nsnap; s++) {
    uint64_t footer = snapshot_footer(T, s);
    const BCIns *pc;
    MSize pos;
    unsigned postcall = (uint8_t)footer;
    if (!postcall) continue;
    pc = (const BCIns *)(uintptr_t)(footer>>8);
    pos = proto_bcpos(pt, pc);
    if (pos+1u != pt->sizebc ||
        bc_op((BCIns)la_load32_acq((const uint32_t *)pc)) != BC_RET1)
      continue;
    f.child = s;
    return_reset_fixture(&f);
    assert(return_admit(&f, 0) && return_admit(&f, 1));
    ret_terminal_maps++;

    for (int postra = 0; postra < 2; postra++) {
      return_test_negative(&f, s, RET_NEG_SWAP, postra, RET_NEG_SWAP);
      return_test_negative(&f, s, RET_NEG_OMIT, postra, RET_NEG_OMIT);
      return_test_negative(&f, s, RET_NEG_NORESTORE, postra, RET_NEG_NORESTORE);
      return_test_negative(&f, s, RET_NEG_A, postra, RET_NEG_A);
      return_test_negative(&f, s, RET_NEG_D, postra, RET_NEG_D);
      return_test_negative(&f, s, RET_NEG_PC, postra, RET_NEG_PC);
      return_test_negative(&f, s, RET_NEG_CSET_SRC, postra, RET_NEG_CSET_SRC);
      return_test_negative(&f, s, RET_NEG_CSET_DST, postra, RET_NEG_CSET_DST);
      return_test_negative(&f, s, RET_NEG_CALL_A, postra, RET_NEG_CALL_A);
      return_test_negative(&f, s, RET_NEG_SLOAD_SLOT, postra, RET_NEG_SLOAD_SLOT);
      return_test_negative(&f, s, RET_NEG_SLOAD_MODE, postra, RET_NEG_SLOAD_MODE);
      return_test_negative(&f, s, RET_NEG_SLOAD_TYPE, postra, RET_NEG_SLOAD_TYPE);
      return_test_negative(&f, s, RET_NEG_NSLOTS, postra, RET_NEG_NSLOTS);
      return_test_negative(&f, s, RET_NEG_TOPSLOT, postra, RET_NEG_TOPSLOT);
      if (s+1u < T->nsnap) {
        return_test_negative(&f, s, RET_NEG_NEXT_PC, postra, RET_NEG_NEXT_PC);
        ret_next_map_bad++;
      }
      return_test_negative(&f, s, RET_NEG_DCE_GUARD, postra,
                           RET_NEG_DCE_GUARD);
      return_test_negative(&f, s, RET_NEG_NEXT_OMIT, postra, RET_NEG_NEXT_OMIT);
      return_test_negative(&f, s, RET_NEG_UNKNOWN_RETURN, postra,
                           RET_NEG_UNKNOWN_RETURN);
      if (strncmp(active_name, "literal-", 8) == 0) {
        return_test_negative(&f, s, RET_NEG_KPRI_SWAP, postra,
                             RET_NEG_KPRI_SWAP);
        return_test_negative(&f, s, RET_NEG_KPRI_RANGE, postra,
                             RET_NEG_KPRI_RANGE);
        return_test_negative(&f, s, RET_NEG_KPRI_ARITH, postra,
                             RET_NEG_KPRI_ARITH);
      }
      if (postra)
        return_test_negative(&f, s, RET_NEG_ALIAS, postra, RET_NEG_ALIAS);
      if (strncmp(active_name, "dead-prefix-", 12) == 0)
        return_test_negative(&f, s, RET_NEG_DEAD_NONNUM, postra,
                             RET_NEG_DEAD_NONNUM);
      if (strncmp(active_name, "comparison-", 11) == 0) {
        return_test_negative(&f, s, RET_NEG_BRANCH_TARGET, postra,
                             RET_NEG_BRANCH_TARGET);
        return_test_negative(&f, s, RET_NEG_BRANCH_OPERAND, postra,
                             RET_NEG_BRANCH_OPERAND);
        return_test_negative(&f, s, RET_NEG_BRANCH_POLARITY, postra,
                             RET_NEG_BRANCH_POLARITY);
        return_test_negative(&f, s, RET_NEG_BRANCH_FUTURE_MAPOFS, postra,
                             RET_NEG_BRANCH_FUTURE_MAPOFS);
        return_test_negative(&f, s, RET_NEG_BRANCH_FUTURE_NENT, postra,
                             RET_NEG_BRANCH_FUTURE_NENT);
        return_test_negative(&f, s, RET_NEG_BRANCH_OMIT_LIVE, postra,
                             RET_NEG_BRANCH_OMIT_LIVE);
      }
    }
  }
  if (!strcmp(active_name, "dead-ignored-actual")) {
    unsigned tested = 0;
    for (SnapNo s = 0; s+1u < T->nsnap; s++) {
      uint64_t footer = snapshot_footer(T, s);
      IRRef at = T->snap[s].ref, end = T->snap[s+1u].ref;
      unsigned missing = 0;
      return_reset_fixture(&f);
      if ((uint8_t)footer || at <= f.loopref || end != at+1u ||
          (const BCIns *)(uintptr_t)(footer>>8) !=
            only_call(trace_startpt_acq(T)) ||
          T->ir[at].o != IR_ADD ||
          (T->ir[at].t.irt & ~IRT_ISPHI) != IRT_INT) continue;
      for (MSize i = 0; i < T->snap[s].nent; i++) {
        IRRef r = snap_ref(T->snapmap[T->snap[s].mapofs+i]);
        RegSP loc;
        if (irref_isk(r)) continue;
        loc = return_value_location(&f, r, s);
        if (ra_noreg(regsp_reg(loc)) && !ra_hasspill(regsp_spill(loc))) missing++;
      }
      assert(missing && return_admit(&f, 0) && return_admit(&f, 1));
      f.child = s;
      f.ir[at].o = IR_ADDOV; f.ir[at].t.irt |= IRT_GUARD;
      return_expect_reject(&f, RET_NEG_CALLMAP_GUARD, 1);
      tested++; ret_callmap_guards++;
    }
    assert(tested == 1);
  }
  assert(ret_terminal_maps > 0);
  free(f.root); free(f.leafpt); free(f.leaf); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

static void run_case(lua_State *L, const ReturnCase *shape)
{
  GCtrace *T = NULL;
  if (!strcmp(shape->name, "comparison-nan"))
    ljt_lua_dostring(L, "arg_payload=2.5");
  else if (!strcmp(shape->name, "comparison-negative-zero"))
    ljt_lua_dostring(L, "arg_payload=-0.0");
  install_case(L, shape);
  assert(lj_trace_arm64_lua_leaf(global_function(L, "leaf_candidate")));
  for (unsigned attempt = 0; attempt < 8 && !T; attempt++) {
    checked(L, NULL, 0, 1, 96); T = root_trace(L);
  }
  if (!T) fprintf(stderr, "%s did not publish\n", shape->name);
  assert(T);
  checked(L, T, 0, 9, 40);
  if (!strcmp(shape->name, "comparison-negative-warm"))
    checked(L, T, 1, 1, 4);
  else
    checked(L, T, 1, 9, 40);
  checked(L, T, 0, 9, 40);
  if (strncmp(shape->name, "comparison-", 11) == 0) {
    int negative_warm = strcmp(shape->name, "comparison-negative-warm") == 0;
    checked(L, T, 0, negative_warm ? 10 : -4, negative_warm ? 14 : 4);
    if (!strcmp(shape->name, "comparison-nan")) {
      ljt_lua_dostring(L, "arg_payload=0/0");
      checked(L, T, 0, 1, 4);
    }
    assert_branch_exit(L, T);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  checked(L, T, 0, 1, 24); gc_reuses++;
  admission(T);
  cases_run++;
  printf("Lua return %s: native terminal exit and reuse verified\n", shape->name);
}

int main(void)
{
  static const char *payloads[] = {
    "nil", "false", "true", "7", ".5", "string.rep('payload',100)",
    "{}", "function() return 7 end", "coroutine.create(function() end)",
    "newproxy(true)", "require('ffi').new('int[1]',17)", NULL
  };
  static const char *names[] = {
    "nil", "false", "true", "int", "num", "string", "table", "function",
    "thread", "userdata", "cdata", "lightuserdata"
  };
  static const ReturnCase layouts[] = {
    {"argument","a,b","return b","i,payload",0},
    {"copy","a,b","local value=b; return value","i,payload",0},
    {"overwrite","a,b","a=b; return a","i,payload",0},
    {"dead-prefix","a,b","local dead=a*3; return b","i,payload",0},
    {"captured","a,b","return b","i,payload",1},
    {"dead-overwrite","a,b","local x=a*3; x=b; return x","i,payload",0}
  };
  static const ReturnCase cases[] = {
    {"literal-nil","a","local value=1; value=nil; return value","i",0},
    {"literal-false","a","local value=1; value=false; return value","i",0},
    {"literal-true","a","local value=1; value=true; return value","i",0},
    {"literal-prefix-overwrite","a",
      "local flag=true; local n=a+1; a=flag; return a","i",0},
    {"missing","a,b","return b","i",0},
    {"missing-all","a,b","return b","",0},
    {"extra","a,b","return b","i,payload,true,nil",0},
    {"earlier-temp","a,b","local x=a*3; a=b; return x","i,payload",0},
    {"compound","a,b","local x=a*3; local y=x+2; a=b; return y","i,payload",0},
    {"constant","a,b","local x=7; a=b; return x","i,payload",0},
    {"dead-chain","a,b","local x=a*3; local y=x+2; return b","i,payload",0},
    {"dead-ignored-actual","a,b,c","local x=a*3; return c","i,i+1,payload",0},
    {"comparison-lt","a","local value=7; if a < 0 then value=1 else value=2 end; return value","i",0},
    {"comparison-le","a","local value=7; if a <= 0 then value=1 else value=2 end; return value","i",0},
    {"comparison-gt","a","local value=7; if a > 0 then value=1 else value=2 end; return value","i",0},
    {"comparison-ge","a","local value=7; if a >= 0 then value=1 else value=2 end; return value","i",0},
    {"comparison-reversed","a","local value=7; if 0 > a then value=1 else value=2 end; return value","i",0},
    {"comparison-positive-zero","a","local value=7; if a < 0.0 then value=1 else value=2 end; return value","i",0},
    {"comparison-negative-zero","a,b","local value=7; if a < b then value=1 else value=2 end; return value","i,payload",0},
    {"comparison-nan","a,b","local value=7; if a < b then value=1 else value=2 end; return value","i/100,payload",0},
    {"comparison-negative-warm","a","local value=7; if a < 0 then value=1 else value=2 end; return value","i-5",0},
    {"comparison-primitive-overwrite","a","local value=true; if a < 0 then value=false else value=true end; return value","i",0},
    {"comparison-arm-liveness","a","local value=7; if a < 0 then value=a else value=2 end; return value","i",0}
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "assert(select(2,s:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5)))");
  for (unsigned p = 0; p < sizeof(payloads)/sizeof(payloads[0]); p++) {
    char source[256];
    if (payloads[p]) {
      snprintf(source, sizeof(source), "arg_payload=%s", payloads[p]);
      ljt_lua_dostring(L, source);
    } else {
      lua_pushlightuserdata(L, (void *)&native_runs); lua_setglobal(L, "arg_payload");
    }
    for (unsigned k = 0; k < sizeof(layouts)/sizeof(layouts[0]); k++) {
      ReturnCase shape = layouts[k];
      char name[64];
      snprintf(name, sizeof(name), "%s-%s", shape.name, names[p]);
      shape.name = name; run_case(L, &shape);
    }
  }
  for (unsigned k = 0; k < sizeof(cases)/sizeof(cases[0]); k++) run_case(L, &cases[k]);
  ljt_lua_dostring(L,
    "assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  assert(ret_terminal_maps == cases_run && ret_alias_bad && ret_callmap_guards == 1);
  for (unsigned m = 0; m < RET_NEG_ALIAS; m++)
    assert(ret_mutation_count[m][0] && ret_mutation_count[m][1]);
  for (unsigned m = 0; m < 6; m++)
    assert(branch_mutation_count[m][0] && branch_mutation_count[m][1]);
  assert(cases_run == 95 && native_runs == 392 && terminal_exits == 95 &&
         same_trace_reuses == 297 && gc_reuses == 95 && primitive_mismatches == 95);
  printf("Lua return exits OK: cases=%u native=%u terminal=%u reuse=%u gc=%u "
         "controlled-old-tags=%u semantic-negative=%u postRA-negative=%u aliases=%u maps=%u callmap-guard=%u\n", cases_run,native_runs,terminal_exits,
         same_trace_reuses,gc_reuses,primitive_mismatches,
         ret_semantic_bad,ret_postra_bad,ret_alias_bad,ret_terminal_maps,ret_callmap_guards);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-return-exits SKIP"); return 0; }
#endif
