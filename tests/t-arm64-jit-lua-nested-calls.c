/* Native evidence for two ordinary pure Lua frames in one ARM64 loop trace. */
#include <stdio.h>
#include <stdlib.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_bc.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lj_asm.h"
#include "lib/lua_fixture_helpers.h"

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

static GCtrace *root_trace(lua_State *L)
{
  GCproto *pt = funcproto(global_function(L, "nested_run"));
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

static lua_Number invoke(lua_State *L, const char *name, lua_Integer first,
                         lua_Integer last)
{
  lua_Number value;
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  if (strcmp(name, "nested_run") == 0) {
    lua_getglobal(L, "nested_outer");
    lua_getglobal(L, "nested_leaf");
  }
  lua_pushinteger(L, first);
  lua_pushinteger(L, last);
  ljt_lua_pcall(L, strcmp(name, "nested_run") == 0 ? 4 : 2, 1,
                "nested Lua call");
  assert(lua_isnumber(L, -1));
  value = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == cframe && lj_tg_load_jit_base(L2TG(L)) == NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return value;
}

typedef struct NestedProof {
  GCtrace *live, copy;
  GCproto *root, *childpt, *root_source, *child_source;
  GCfunc *child, *child_source_fn;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef nins, function_ref, link_ref;
  SnapNo child_snap;
  BCReg base, funcslot, linkslot;
} NestedProof;

static GCproto *nested_copy_proto(const GCproto *pt)
{
  GCproto *copy = malloc(pt->sizept);
  assert(copy);
  memcpy(copy, pt, pt->sizept);
  return copy;
}

static void nested_proof_reset(NestedProof *f, lua_State *L, GCtrace *T)
{
  MSize startpos;
  SnapNo s;
  f->live = T;
  f->root_source = trace_startpt_acq(T);
  f->child_source = funcproto(f->child_source_fn);
  startpos = proto_bcpos(f->root_source, trace_startpc_acq(T));
  memcpy(f->root, f->root_source, f->root_source->sizept);
  memcpy(f->childpt, f->child_source, f->child_source->sizept);
  setmref(f->root->jit_startins, proto_bc(f->root)+f->root->sizebc);
  setmref(f->childpt->jit_startins, proto_bc(f->childpt)+f->childpt->sizebc);
  memset(f->child, 0, sizeLfunc(0));
  memcpy(f->child, f->child_source_fn, sizeLfunc(0));
  setmref(f->child->l.pc, proto_bc(f->childpt));
  memcpy(f->ir+T->nk, T->ir+T->nk,
         (T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->function_ref], obj2gco(f->child));
  memcpy(f->snap, T->snap, T->nsnap*sizeof(SnapShot));
  memcpy(f->map, T->snapmap, T->nsnapmap*sizeof(SnapEntry));
  f->copy = *T;
  f->copy.ir = f->ir; f->copy.snap = f->snap; f->copy.snapmap = f->map;
  trace_startpt_rel(&f->copy, f->root);
  setmref(f->copy.startpc, proto_bc(f->root)+startpos);
  proto_bc(f->root)[startpos] = T->startins;
  for (s = 0; s < T->nsnap; s++) {
    SnapShot *snap = &f->snap[s];
    uint64_t footer;
    MSize pos;
    memcpy(&footer, &T->snapmap[snap->mapofs+snap->nent], sizeof(footer));
    if ((uint8_t)footer) {
      pos = proto_bcpos(f->child_source,
                        (const BCIns *)(uintptr_t)(footer>>8));
      assert(pos < f->childpt->sizebc);
      footer = (u64ptr(proto_bc(f->childpt)+pos)<<8)|(uint8_t)footer;
      for (MSize i = 0; i < snap->nent; i++) {
        SnapEntry entry = T->snapmap[snap->mapofs+i];
        if ((entry & 0x00ff0000u) == SNAP_FRAME) {
          IRRef link = snap_ref(entry);
          const BCIns *retpc = (const BCIns *)(uintptr_t)T->ir[link+1u].tv.u64;
          MSize retpos = proto_bcpos(f->root_source, retpc);
          assert(T->ir[link].o == IR_KNUM && retpos < f->root->sizebc);
          f->ir[link+1u].tv.u64 = u64ptr(proto_bc(f->root)+retpos);
        }
      }
    } else {
      pos = proto_bcpos(f->root_source,
                        (const BCIns *)(uintptr_t)(footer>>8));
      assert(pos < f->root->sizebc);
      footer = u64ptr(proto_bc(f->root)+pos)<<8;
    }
    memcpy(&f->map[snap->mapofs+snap->nent], &footer, sizeof(footer));
  }
  memset(f->J, 0, sizeof(*f->J));
  f->J->L = L; f->J->pt = f->root;
  f->J->startpc = trace_startpc_acq(&f->copy);
  f->J->baseslot = 1u+LJ_FR2;
  f->nins = T->nins;
  while (f->nins > T->nk &&
         (f->ir[f->nins-1u].o == IR_RENAME ||
          f->ir[f->nins-1u].o == IR_NOP)) f->nins--;
  f->copy.nins = f->nins;
  for (IRRef r = REF_FIRST; r < f->nins; r++)
    if (f->ir[r].o == IR_LOOP) f->J->loopref = r;
  f->view = (LJArm64PostRAView){0};
  f->view.ir = f->ir; f->view.owner = &f->copy; f->view.owner_private = 1;
  f->view.snap = f->snap; f->view.snapmap = f->map;
  f->view.proto = f->root; f->view.proto_bc = proto_bc(f->root);
  f->view.startpc = trace_startpc_acq(&f->copy);
  f->view.nins = T->nins; f->view.nk = T->nk;
  f->view.nsnap = T->nsnap; f->view.nsnapmap = T->nsnapmap;
  f->view.spadjust = T->spadjust;
  f->view.proto_sizebc = f->root->sizebc;
  f->view.proto_numparams = f->root->numparams;
  f->view.root_topslot = T->topslot; f->view.startins = T->startins;
}

static int nested_proof_admit(NestedProof *f, int postra)
{
  if (postra) return lj_asm_arm64_postra_admit(&f->view, NULL);
  f->copy.nins = f->nins;
  return lj_asm_arm64_ir_admit(f->J, &f->copy, NULL);
}

static MSize nested_map_entry(const NestedProof *f, SnapNo s, BCReg slot)
{
  const SnapShot *snap = &f->snap[s];
  for (MSize i = 0; i < snap->nent; i++)
    if (snap_slot(f->map[snap->mapofs+i]) == slot) return snap->mapofs+i;
  return UINT32_MAX;
}

static void nested_proof_mutations(lua_State *L, GCtrace *T)
{
  NestedProof f = {0};
  uint64_t footer;
  IRRef wrong_function = 0;
  IRRef borrowed_guard = 0, borrowed_function = 0, borrowed_constant = 0;
  GCfunc *expected_inner = global_function(L, "nested_leaf");
  MSize funcentry, argentry;
  unsigned postra;
  f.live = T;
  f.root_source = trace_startpt_acq(T);
  f.ir = calloc(T->nins+1u, sizeof(IRIns));
  f.snap = malloc(T->nsnap*sizeof(SnapShot));
  f.map = malloc(T->nsnapmap*sizeof(SnapEntry));
  f.J = calloc(1, sizeof(jit_State));
  assert(f.ir && f.snap && f.map && f.J);
  for (SnapNo s = 0; s < T->nsnap && !f.child_source_fn; s++) {
    const SnapShot *snap = &T->snap[s];
    MSize frames = 0;
    memcpy(&footer, &T->snapmap[snap->mapofs+snap->nent], sizeof(footer));
    if (!(uint8_t)footer) continue;
    for (MSize i = 0; i < snap->nent; i++)
      if ((T->snapmap[snap->mapofs+i] & 0x00ff0000u) == SNAP_FRAME) frames++;
    if (!frames) continue;
    for (MSize i = 0; i < snap->nent; i++) {
      SnapEntry entry = T->snapmap[snap->mapofs+i];
      if ((entry & 0x00ff0000u) == SNAP_FRAME) {
        IRRef ref = snap_ref(entry);
        f.child_snap = s; f.linkslot = snap_slot(entry);
        f.link_ref = ref; f.base = f.linkslot+1u;
        f.funcslot = f.linkslot-1u;
        f.function_ref = 0;
        for (MSize j = 0; j < snap->nent; j++) {
          SnapEntry fnentry = T->snapmap[snap->mapofs+j];
          if (snap_slot(fnentry) == f.funcslot &&
              !(fnentry & 0x00ff0000u)) f.function_ref = snap_ref(fnentry);
        }
        assert(f.function_ref && T->ir[f.function_ref].o == IR_KGC);
        f.child_source_fn = gco2func(ir_kgc_acq(&T->ir[f.function_ref]));
        f.child_source = funcproto(f.child_source_fn);
        break;
      }
    }
  }
  assert(f.child_source_fn && f.child_source_fn != NULL);
  assert(f.child_source->numparams == 2u);
  f.root = nested_copy_proto(f.root_source);
  f.childpt = nested_copy_proto(f.child_source);
  f.child = calloc(1, sizeLfunc(0));
  assert(f.root && f.childpt && f.child);
  for (IRRef r = T->nk; r < REF_TRUE; r++) {
    if (T->ir[r].o == IR_KGC && T->ir[r].t.irt == IRT_FUNC &&
        ir_kgc_acq(&T->ir[r]) != obj2gco(f.child_source_fn)) {
      wrong_function = r; break;
    }
    if (T->ir[r].o == IR_KGC || T->ir[r].o == IR_KNUM) r += LJ_GC64;
  }
  assert(wrong_function);
  for (IRRef r = REF_FIRST; r < T->nins; r++) {
    IRIns guard = ir_load_acq(&T->ir[r]);
    IRRef constant;
    if (guard.o != IR_EQ || guard.t.irt != (IRT_FUNC|IRT_GUARD)) continue;
    constant = irref_isk(guard.op1) ? guard.op1 : guard.op2;
    if (!irref_isk(constant) || constant < T->nk || constant >= REF_TRUE ||
        T->ir[constant].o != IR_KGC ||
        ir_kgc_acq(&T->ir[constant]) != obj2gco(expected_inner)) continue;
    borrowed_guard = r;
    borrowed_constant = constant;
    borrowed_function = guard.op1 == constant ? guard.op2 : guard.op1;
    break;
  }
  assert(borrowed_guard && borrowed_function >= REF_FIRST);
  assert(borrowed_constant+1u < REF_TRUE);
  assert(f.function_ref+1u < REF_TRUE);
  nested_proof_reset(&f, L, T);
  memcpy(&footer, &f.map[f.snap[f.child_snap].mapofs+
                          f.snap[f.child_snap].nent], sizeof(footer));
  assert(nested_proof_admit(&f, 0) && nested_proof_admit(&f, 1));
  funcentry = nested_map_entry(&f, f.child_snap, f.funcslot);
  argentry = nested_map_entry(&f, f.child_snap, f.base);
  assert(funcentry != UINT32_MAX && argentry != UINT32_MAX);
  for (postra = 0; postra < 2; postra++) {
    /* Wrong child function identity. */
    nested_proof_reset(&f, L, T);
    f.map[funcentry] = SNAP(f.funcslot, 0,
                            wrong_function ? wrong_function : REF_NIL);
    assert(!nested_proof_admit(&f, postra));
    /* The second GC64 KGC word is payload, not a frame identity constant. */
    nested_proof_reset(&f, L, T);
    f.map[funcentry] = SNAP(f.funcslot, 0, f.function_ref+1u);
    assert(!nested_proof_admit(&f, postra));
    nested_proof_reset(&f, L, T);
    assert(nested_proof_admit(&f, postra));
    /* A borrowed callable EQ operand below nk is not an IR constant slot. */
    nested_proof_reset(&f, L, T);
    if (f.ir[borrowed_guard].op1 == borrowed_function)
      f.ir[borrowed_guard].op2 = (IRRef)(T->nk-1u);
    else
      f.ir[borrowed_guard].op1 = (IRRef)(T->nk-1u);
    assert(!nested_proof_admit(&f, postra));
    /* A KGC payload word is not an independently addressable constant. */
    nested_proof_reset(&f, L, T);
    if (f.ir[borrowed_guard].op1 == borrowed_function)
      f.ir[borrowed_guard].op2 = (IRRef)(borrowed_constant+1u);
    else
      f.ir[borrowed_guard].op1 = (IRRef)(borrowed_constant+1u);
    assert(!nested_proof_admit(&f, postra));
    /* Non-numeric first formal in the child frame (FR2 geometry source). */
    nested_proof_reset(&f, L, T);
    f.map[argentry] = SNAP(f.base, 0, REF_NIL);
    assert(!nested_proof_admit(&f, postra));
    /* Malformed frame-to-caller PC link. */
    nested_proof_reset(&f, L, T);
    f.ir[f.link_ref].o = IR_KNUM;
    f.ir[f.link_ref].t.irt = IRT_NUM;
    f.ir[f.link_ref+1u].tv.u64 = u64ptr(proto_bc(f.root)+f.root->sizebc);
    assert(!nested_proof_admit(&f, postra));
    /* Bad inline child continuation PC/base pair. */
    nested_proof_reset(&f, L, T);
    footer = (footer & ~UINT64_C(0xff)) | ((uint8_t)f.base ^ 1u);
    memcpy(&f.map[f.snap[f.child_snap].mapofs+
                    f.snap[f.child_snap].nent], &footer, sizeof(footer));
    assert(!nested_proof_admit(&f, postra));
    /* The actual scalar argument starts after the FR2 function-frame pad. */
    nested_proof_reset(&f, L, T);
    {
      MSize p;
      for (p = 1; p+1u < f.childpt->sizebc; p++)
        if (bc_op(proto_bc(f.childpt)[p]) == BC_CALL) break;
      assert(p+1u < f.childpt->sizebc &&
             bc_c(proto_bc(f.childpt)[p]) == 2u);
      setbc_c(&proto_bc(f.childpt)[p], 3u);
    }
    assert(!nested_proof_admit(&f, postra));
    /* Bind the nested return through the exact CALL destination copied to the
    ** parent's RET1 source; an unrelated local must not satisfy the result. */
    nested_proof_reset(&f, L, T);
    {
      MSize p, callpos;
      for (callpos = 1; callpos+1u < f.childpt->sizebc; callpos++)
        if (bc_op(proto_bc(f.childpt)[callpos]) == BC_CALL) break;
      assert(callpos+1u < f.childpt->sizebc);
      for (p = callpos+1u; p+1u < f.childpt->sizebc; p++) {
        BCIns ins = (BCIns)la_load32_acq(
          (const uint32_t *)&proto_bc(f.childpt)[p]);
        if (bc_op(ins) == BC_CGET && bc_a(ins) == 3u &&
            bc_d(ins) == bc_a(proto_bc(f.childpt)[callpos])) break;
      }
      assert(p+1u < f.childpt->sizebc);
      setbc_d(&proto_bc(f.childpt)[p], 0u);
    }
    assert(!nested_proof_admit(&f, postra));
  }
  free(f.root); free(f.childpt); free(f.child); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

static SnapNo nested_identity_exit(const GCtrace *T, GCfunc *expected)
{
  for (IRRef r = REF_FIRST; r < T->nins; r++) {
    IRIns guard = ir_load_acq(&T->ir[r]);
    IRRef constant;
    if (guard.o != IR_EQ || guard.t.irt != (IRT_FUNC|IRT_GUARD)) continue;
    constant = irref_isk(guard.op1) ? guard.op1 : guard.op2;
    if (!irref_isk(constant) || T->ir[constant].o != IR_KGC ||
        ir_kgc_acq(&T->ir[constant]) != obj2gco(expected)) continue;
    for (SnapNo s = 0; s < T->nsnap; s++)
      if (snap_ref_acq(&T->snap[s]) == r) return s;
  }
  return UINT16_MAX;
}

static void nested_force_identity_exit(lua_State *L, GCtrace *T,
                                       const char *global,
                                       const char *replacement)
{
  GCfunc *expected = global_function(L, global);
  SnapNo exit = nested_identity_exit(T, expected);
  lua_Number oracle;
  assert(exit != UINT16_MAX);
  lua_getglobal(L, global);
  lua_getglobal(L, replacement);
  lua_setglobal(L, global);
  oracle = invoke(L, "nested_oracle", 9, 40);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L, "nested_run", 9, 40) == oracle);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno) &&
         lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno &&
         lj_trace_test_first_exitno() == exit);
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  lua_pushvalue(L, -1);
  lua_setglobal(L, global);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  GCtrace *T = NULL;
  unsigned attempt;
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "assert(select(2,s:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "function nested_leaf(x) return x+0.25 end; "
    "function nested_leaf_alt(x) return x+0.25 end; "
    "function nested_outer(x,g) local v=g(x); return v end; "
    "function nested_outer_alt(x,g) local v=g(x); return v end; "
    "function nested_run(f,g,first,last) local sum=.5; "
    "for i=first,last do sum=sum+f(i,g) end; "
    "return sum end; "
    "function nested_oracle(first,last) local sum=.5; "
    "for i=first,last do sum=sum+(i+0.25) end; return sum end; "
    "jit.off(nested_oracle,true); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000')");
  for (attempt = 0; attempt < 8 && !T; attempt++) {
    assert(invoke(L, "nested_run", 1, 96) == invoke(L, "nested_oracle", 1, 96));
    T = root_trace(L);
  }
  if (!T || !lj_trace_arm64_lua_leaf(global_function(L, "nested_outer"))) {
    abort();
  }
  nested_proof_mutations(L, T);
  lj_trace_test_root_entry_reset();
  assert(invoke(L, "nested_run", 9, 40) == invoke(L, "nested_oracle", 9, 40));
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno);
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  lua_gc(L, LUA_GCCOLLECT, 0);
  lj_trace_test_root_entry_reset();
  assert(invoke(L, "nested_run", 1, 24) == invoke(L, "nested_oracle", 1, 24));
  assert(root_trace(L) == T && lj_trace_test_root_entry_successes() > 0);
  nested_force_identity_exit(L, T, "nested_leaf", "nested_leaf_alt");
  nested_force_identity_exit(L, T, "nested_outer", "nested_outer_alt");
  ljt_lua_dostring(L,
    "assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  puts("nested Lua calls OK: nested native entry, identity exits and post-GC reuse");
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-nested-calls SKIP"); return 0; }
#endif
