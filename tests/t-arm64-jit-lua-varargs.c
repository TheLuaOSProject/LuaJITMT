/* Fixed ordinary CALL into bounded FUNCV/VARG leaves, exact child overflow
** continuation, paired-frame geometry, peer lifetime, and GC/reuse. */
#include <stdio.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_frame.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/proto_fixture_helpers.h"

static unsigned native_entries, guard_exits, semantic_bad, postra_bad;

typedef struct VarargMap {
  SnapNo snap;
  MSize lower_marker, upper_marker, topslot;
  IRRef lower_func, upper_func, lower_pc, upper_delta;
  const BCIns *retpc;
  BCIns call, varg;
  MSize nargs, requested, available, callerbase, callfunc, childbase;
} VarargMap;

static GCfunc *global_function(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1)); fn = funcV(L->top-1);
  assert(isluafunc(fn)); lua_pop(L, 1);
  return fn;
}

static lua_Number invoke(lua_State *L, const char *runner, const char *leaf,
                         int n, lua_Integer bias)
{
  lua_Number result;
  void *cframe = L->cframe;
  lua_getglobal(L, runner); lua_getglobal(L, leaf);
  lua_pushinteger(L, n); lua_pushinteger(L, bias);
  ljt_lua_pcall(L, 3, 1, "vararg Lua leaf call");
  assert(lua_isnumber(L, -1)); result = lua_tonumber(L, -1); lua_pop(L, 1);
  assert(L->cframe == cframe && lj_tg_load_jit_base(L2TG(L)) == NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static GCtrace *root_trace(lua_State *L)
{
  GCproto *pt = funcproto(global_function(L, "leaf_run"));
  BCIns *bc = proto_bc(pt);
  for (MSize i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_JFORL) {
      GCtrace *T = traceref_safe(L2J(L), bc_d(ins));
      if (T && trace_runnable_acq(T, bc_d(ins)) && !T->root &&
          trace_startpt_acq(T) == pt && T->linktype == LJ_TRLINK_LOOP &&
          T->link == T->traceno) return T;
    }
  }
  return NULL;
}

static MSize find_slot(const GCtrace *T, const SnapShot *snap, MSize slot)
{
  MSize i, found = UINT32_MAX;
  for (i = 0; i < snap->nent; i++) {
    MSize at = snap->mapofs+i;
    if (snap_slot(T->snapmap[at]) == slot) {
      assert(found == UINT32_MAX); found = at;
    }
  }
  return found;
}

static int child_vararg_snapshot(const GCtrace *T, SnapNo s,
                                 VarargMap *out)
{
  const SnapShot *snap = &T->snap[s];
  MSize i, nmarker = 0;
  uint64_t pcbase;
  GCproto *rootpt=(GCproto *)trace_startpt_acq((GCtrace *)T);
  IRRef refs[2];
  MSize slots[2];
  BCIns call, varg;
  int forward;
  const BCIns *retpc;
  IRRef lowerfn, upperfn;
#define MAP_FAIL() return 0
  if (snap->mapofs > T->nsnapmap || snap->nent > T->nsnapmap-snap->mapofs ||
      T->nsnapmap-snap->mapofs-snap->nent < 2u) MAP_FAIL();
  for (i = 0; i < snap->nent; i++) {
    SnapEntry entry = T->snapmap[snap->mapofs+i];
    if ((entry & 0x00ff0000u) == SNAP_FRAME) {
      if (nmarker == 2u) MAP_FAIL();
      slots[nmarker] = snap_slot(entry);
      refs[nmarker] = snap_ref(entry); nmarker++;
    }
  }
  if (nmarker != 2u || slots[1] <= slots[0] ||
      slots[0] < 1u+LJ_FR2+1u || slots[1] < slots[0]+2u+LJ_FR2)
    MAP_FAIL();
  if (!irref_isk(refs[0]) || refs[0] < T->nk || refs[0] >= REF_TRUE ||
      T->ir[refs[0]].o != IR_KNUM ||
      !irref_isk(refs[1]) || refs[1] < T->nk || refs[1] >= REF_TRUE ||
      T->ir[refs[1]].o != IR_KNUM) MAP_FAIL();
  retpc = (const BCIns *)(uintptr_t)T->ir[refs[0]+1u].tv.u64;
  if (!rootpt || retpc <= proto_bc(rootpt) ||
      retpc >= proto_bc(rootpt)+rootpt->sizebc)
    MAP_FAIL();
  call = (BCIns)la_load32_acq((const uint32_t *)(retpc-1));
  if (bc_op(call) != BC_CALL || bc_b(call) != 2u || !bc_c(call)) MAP_FAIL();
  if (T->ir[refs[1]+1u].tv.u64 !=
      8u*(bc_c(call)-1u+1u+LJ_FR2)+FRAME_VARG) MAP_FAIL();
  varg = 0;
  {
    MSize lower_entry=find_slot(T,snap,slots[0]-1u);
    MSize upper_entry=find_slot(T,snap,slots[1]-1u);
    if (lower_entry==UINT32_MAX || upper_entry==UINT32_MAX) MAP_FAIL();
    lowerfn=snap_ref(T->snapmap[lower_entry]);
    upperfn=snap_ref(T->snapmap[upper_entry]);
  }
  if (lowerfn != upperfn || !irref_isk(lowerfn) || lowerfn < T->nk ||
      lowerfn >= REF_TRUE || T->ir[lowerfn].o != IR_KGC ||
      !isluafunc(ir_kfunc(&T->ir[lowerfn]))) MAP_FAIL();
  {
    const GCproto *leaf = funcproto(ir_kfunc(&T->ir[lowerfn]));
    MSize p;
    if (!(leaf->flags & PROTO_VARARG) || leaf->numparams != 0) MAP_FAIL();
    for (p = 1; p+1u < leaf->sizebc; p++) {
      BCIns word = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(leaf)[p]);
      if (bc_op(word) == BC_VARG) {
        if (varg) MAP_FAIL();
        varg = word;
      }
    }
    forward = bc_b(varg) == 0u && leaf->sizebc == 4u &&
      bc_op((BCIns)la_load32_acq((const uint32_t *)&proto_bc(leaf)[1])) == BC_UGET &&
      bc_op((BCIns)la_load32_acq((const uint32_t *)&proto_bc(leaf)[3])) == BC_CALLMT &&
      lj_trace_arm64_lua_leaf(ir_kfunc(&T->ir[lowerfn]));
    if (!varg || bc_c(varg) != 0 ||
        (bc_b(varg) == 0u ? !forward : bc_b(varg) < 2u))
      MAP_FAIL();
  }
  memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
  if ((uint8_t)pcbase+2u != slots[1]+1u) MAP_FAIL();
  if (forward) {
    MSize forward_pos;
    const GCproto *fpt=funcproto(ir_kfunc(&T->ir[lowerfn]));
    const BCIns *forward_pc=(const BCIns *)(uintptr_t)(pcbase>>8);
    if (forward_pc<proto_bc(fpt) ||
        forward_pc>=proto_bc(fpt)+fpt->sizebc) MAP_FAIL();
    forward_pos=(MSize)(forward_pc-proto_bc(fpt));
    if (forward_pos != 3u ||
        (MSize)slots[1]+1u+bc_a(varg)+bc_c(call)-1u > snap->nslots)
      MAP_FAIL();
  }
  if (out) {
    out->snap=s; out->lower_marker=slots[0]; out->upper_marker=slots[1];
    out->topslot=snap->topslot; out->lower_func=lowerfn; out->upper_func=upperfn;
    out->lower_pc=refs[0]; out->upper_delta=refs[1]; out->retpc=retpc;
    out->call=call; out->varg=varg; out->nargs=bc_c(call)-1u;
    out->requested=bc_b(varg) ? bc_b(varg)-1u : out->nargs;
    out->available=out->nargs;
    out->callerbase=1u+LJ_FR2; out->callfunc=out->callerbase+bc_a(call);
    out->childbase=slots[1]+1u;
    /* All actual arguments are part of the fixed CALL source window, even
    ** when VARG truncates them.  Only the intersection is copied to VARG's
    ** result window; absent requested values are padded with REF_NIL. */
    for (i=0; i<out->available; i++) {
      MSize source=find_slot(T,snap,out->callfunc+1u+LJ_FR2+i);
      if (source==UINT32_MAX || (T->snapmap[source] & 0x00ff0000u)) MAP_FAIL();
      if (i<out->requested && !forward) {
        MSize dest=find_slot(T,snap,out->childbase+bc_a(varg)+i);
        if (dest==UINT32_MAX || (T->snapmap[dest] & 0x00ff0000u) ||
            snap_ref(T->snapmap[source]) != snap_ref(T->snapmap[dest])) MAP_FAIL();
      }
    }
    for (; i<out->requested; i++) {
      if (!forward) {
        MSize dest=find_slot(T,snap,out->childbase+bc_a(varg)+i);
        if (dest!=UINT32_MAX &&
            ((T->snapmap[dest] & 0x00ff0000u) ||
             snap_ref(T->snapmap[dest])!=REF_NIL)) MAP_FAIL();
      }
    }
  }
  return 1;
}

#undef MAP_FAIL

static VarargMap find_vararg_map(const GCtrace *T)
{
  VarargMap result={0};
  SnapNo s;
  for (s=0; s<T->nsnap; s++)
    if (child_vararg_snapshot(T,s,&result)) return result;
  assert(0 && "paired CALL/FRAME_VARG snapshot missing");
  return result;
}

static SnapNo child_addov_exit(const GCtrace *T)
{
  SnapNo s;
  for (s=0; s<T->nsnap; s++) {
    IRRef ref=T->snap[s].ref;
    if (child_vararg_snapshot(T,s,NULL) && ref>=REF_FIRST && ref<T->nins &&
        T->ir[ref].o==IR_ADDOV && irt_isguard(T->ir[ref].t)) return s;
  }
  assert(0 && "native child ADDOV exit missing"); return 0;
}

static void native_call(lua_State *L, GCtrace *T, int n, lua_Integer bias,
                        MSize nargs, MSize requested,
                        int expect_child_overflow, int expect_child_map)
{
  lua_Number expected=invoke(L,"leaf_oracle_run","leaf_oracle",n,bias);
  SnapNo childexit=expect_child_overflow ? child_addov_exit(T) : 0;
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"leaf_run","leaf",n,bias)==expected);
  assert(root_trace(L)==T && trace_runnable_acq(T,T->traceno));
  assert(lj_trace_test_root_entry_successes()>0);
  if (expect_child_overflow) {
    assert(lj_trace_test_exit_calls()>0 &&
           lj_trace_test_first_exit_parent()==T->traceno);
    assert(lj_trace_test_first_exitno()==childexit);
  }
  if (expect_child_map) {
    VarargMap map=find_vararg_map(T);
    assert(map.nargs==nargs && map.requested==requested &&
           map.available==nargs && map.childbase==map.upper_marker+1u);
  }
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  native_entries++;
  if (expect_child_overflow) guard_exits++;
}


static void assert_forward_trace_bound(lua_State *L, GCtrace *T,
                                       GCfunc *forward, GCfunc *target)
{
  GCproto *runner=funcproto(global_function(L,"leaf_run"));
  assert(T && forward && target);
  assert(trace_startpt_acq(T)==runner && T->linktype==LJ_TRLINK_LOOP &&
         T->link==T->traceno && !T->root);
  assert(lj_trace_arm64_lua_leaf_forward_call(forward,target,3u));
}

static void install_case(lua_State *L, const char *body, const char *runner)
{
  lua_getglobal(L,"install_vararg_case");
  lua_pushstring(L,body); lua_pushstring(L,runner);
  ljt_lua_pcall(L,2,0,"install fixed vararg case");
}

static void install_forward_case(lua_State *L)
{
  ljt_lua_dostring(L,
    "install_vararg_forward_case();");
}

typedef struct VarargFixture {
  GCtrace *live, copy;
  GCproto *pt, *leafpt;
  GCfunc *leaf;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  VarargMap source;
  MSize nins, source_arg[2], child_arg[2];
} VarargFixture;

static GCproto *copy_proto(const GCproto *pt)
{
  return ljt_proto_clone_alloc(pt);
}

static void vararg_fixture_reset(VarargFixture *f, lua_State *L)
{
  GCtrace *T=f->live;
  GCproto *root=trace_startpt_acq(T), *leafsource, *targetsource=NULL;
  GCfunc *source=ir_kfunc(&T->ir[f->source.lower_func]);
  MSize startpos=proto_bcpos(root,trace_startpc_acq(T));
  SnapNo s;
  leafsource=funcproto(source);
  if (lj_funcL_nupvalues(&source->l)) {
    TValue target;
    lj_tv_load_acq(&target,uvval(func_uv_acq(&source->l,0)));
    assert(tvisfunc(&target) && isluafunc(funcV(&target)));
    targetsource=funcproto(funcV(&target));
  }
  ljt_proto_clone_reset(f->pt, root);
  ljt_proto_clone_reset(f->leafpt, leafsource);
  memset(f->leaf,0,sizeLfunc(lj_funcL_nupvalues(&source->l)));
  memcpy(f->leaf,source,sizeLfunc(lj_funcL_nupvalues(&source->l)));
  setmref(f->leaf->l.pc,proto_bc(f->leafpt));
  memcpy(f->ir+T->nk,T->ir+T->nk,(T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->source.lower_func],obj2gco(f->leaf));
  memcpy(f->snap,T->snap,T->nsnap*sizeof(SnapShot));
  memcpy(f->map,T->snapmap,T->nsnapmap*sizeof(SnapEntry));
  f->copy=*T; f->copy.ir=f->ir; f->copy.snap=f->snap; f->copy.snapmap=f->map;
  trace_startpt_rel(&f->copy,f->pt);
  setmref(f->copy.startpc,proto_bc(f->pt)+startpos);
  proto_bc(f->pt)[startpos]=T->startins;
  for (IRRef r=T->nk;r+1u<T->nins;r++) {
    if (T->ir[r].o==IR_KNUM) {
      uintptr_t pc=(uintptr_t)T->ir[r+1u].tv.u64;
      const GCproto *owners[3]={root,leafsource,targetsource};
      GCproto *copies[2]={f->pt,f->leafpt};
      for (unsigned p=0;p<3;p++) {
        if (p==2u && targetsource) {
          uintptr_t lo=(uintptr_t)proto_bc(targetsource);
          uintptr_t hi=(uintptr_t)(proto_bc(targetsource)+targetsource->sizebc);
          if (pc>=lo && pc<=hi) break;
          continue;
        }
        uintptr_t lo=(uintptr_t)proto_bc(owners[p]);
        uintptr_t hi=(uintptr_t)(proto_bc(owners[p])+owners[p]->sizebc);
        if (pc>=lo && pc<=hi) {
          MSize pos=(MSize)((pc-lo)/sizeof(BCIns));
          f->ir[r+1u].tv.u64=u64ptr(proto_bc(copies[p])+pos); break;
        }
      }
      r++;
    }
  }
  for (s=0;s<T->nsnap;s++) {
    SnapShot *snap=&f->snap[s];
    uint64_t footer;
    const BCIns *pc;
    uintptr_t at,lo;
    MSize pos;
    memcpy(&footer,&T->snapmap[snap->mapofs+snap->nent],sizeof(footer));
    pc=snap_pc(&T->snapmap[snap->mapofs+snap->nent]); at=(uintptr_t)pc;
    if ((uint8_t)footer) {
      lo=(uintptr_t)proto_bc(leafsource);
      if (at>=lo && at<lo+leafsource->sizebc*sizeof(BCIns)) {
        pos=(MSize)((at-lo)/sizeof(BCIns));
        footer=(u64ptr(proto_bc(f->leafpt)+pos)<<8)|(uint8_t)footer;
      } else if (targetsource && at>=(uintptr_t)proto_bc(targetsource) &&
          at<(uintptr_t)(proto_bc(targetsource)+targetsource->sizebc)) {
        /* The inlined tail target remains live and unmodified. */
      } else {
        assert(0 && "child footer outside copied forwarder/live target");
      }
    } else {
      lo=(uintptr_t)proto_bc(root);
      if (at>=lo && at<lo+root->sizebc*sizeof(BCIns)) {
        pos=(MSize)((at-lo)/sizeof(BCIns));
        footer=u64ptr(proto_bc(f->pt)+pos)<<8;
      } else if (targetsource && at>=(uintptr_t)proto_bc(targetsource) &&
          at<(uintptr_t)(proto_bc(targetsource)+targetsource->sizebc)) {
        /* Target source remains live and unmodified in this copied proof. */
      } else {
        assert(0 && "snapshot PC outside copied root/forwarder or live target");
      }
    }
    memcpy(&f->map[snap->mapofs+snap->nent],&footer,sizeof(footer));
  }
  memset(f->J,0,sizeof(*f->J)); f->J->L=L; f->J->pt=f->pt;
  f->J->startpc=trace_startpc_acq(&f->copy); f->J->baseslot=1u+LJ_FR2;
  f->nins=T->nins;
  while (f->nins>T->nk &&
         (f->ir[f->nins-1u].o==IR_RENAME || f->ir[f->nins-1u].o==IR_NOP))
    f->nins--;
  f->copy.nins=f->nins;
  for (IRRef r=REF_FIRST;r<f->nins;r++)
    if (f->ir[r].o==IR_LOOP) f->J->loopref=r;
  f->view=(LJArm64PostRAView){0}; f->view.ir=f->ir;
  f->view.owner=&f->copy; f->view.owner_private=1;
  f->view.snap=f->snap; f->view.snapmap=f->map;
  f->view.proto=f->pt; f->view.proto_bc=proto_bc(f->pt);
  f->view.startpc=trace_startpc_acq(&f->copy);
  f->view.nins=T->nins; f->view.nk=T->nk;
  f->view.nsnap=T->nsnap; f->view.nsnapmap=T->nsnapmap;
  f->view.spadjust=T->spadjust; f->view.proto_sizebc=f->pt->sizebc;
  f->view.proto_numparams=f->pt->numparams;
  f->view.root_topslot=T->topslot; f->view.startins=T->startins;
}

static int vararg_fixture_admit(VarargFixture *f, int postra)
{
  if (postra) return lj_asm_arm64_postra_admit(&f->view,NULL);
  f->copy.nins=f->nins;
  return lj_asm_arm64_ir_admit(f->J,&f->copy,NULL);
}

enum { VM_CALL_ARGS, VM_CALL_RESULTS, VM_CALL_PC, VM_VARG_B, VM_VARG_C,
       VM_FRAME_DELTA, VM_FUNCTION, VM_SOURCE_ARGUMENT, VM_DEST_ARGUMENT,
       VM_FRAME_SLOT, VM_COUNT_BASE, VM_CALLMT_A, VM_CALLMT_B,
       VM_CALLMT_C, VM_UGET_D, VM_COUNT_FORWARD };

static void vararg_fixture_mutate(VarargFixture *f, unsigned mutation)
{
  SnapShot *snap=&f->snap[f->source.snap];
  MSize callpos=proto_bcpos(trace_startpt_acq(f->live),f->source.retpc)-1u;
  BCIns *call=&proto_bc(f->pt)[callpos];
  BCIns *varg=NULL, *callmt=NULL, *uget=NULL;
  /* Locate vararg/tail bytecodes in the copied source rather than relying on
  ** fixed PCs. */
  for (MSize p=1;p<f->leafpt->sizebc;p++) {
    BCIns *ins=&proto_bc(f->leafpt)[p];
    if (bc_op(*ins)==BC_VARG) varg=ins;
    if (bc_op(*ins)==BC_CALLMT) callmt=ins;
    if (bc_op(*ins)==BC_UGET) uget=ins;
  }
  assert(varg);
  if (mutation>=VM_CALLMT_A) assert(callmt && uget);
  switch (mutation) {
  case VM_CALL_ARGS: setbc_c(call,bc_c(*call)+1u); break;
  case VM_CALL_RESULTS: setbc_b(call,bc_b(*call)+1u); break;
  case VM_CALL_PC:
    f->ir[f->source.lower_pc+1u].tv.u64=u64ptr(call);
    break;
  case VM_VARG_B: setbc_b(varg,bc_b(*varg) ? bc_b(*varg)-1u : 2u); break;
  case VM_VARG_C: setbc_c(varg,1u); break;
  case VM_FRAME_DELTA: f->ir[f->source.upper_delta+1u].tv.u64+=8u; break;
  case VM_FUNCTION:
    {
      int changed=0;
      for (MSize i=0;i<snap->nent;i++) {
        MSize at=snap->mapofs+i;
        if (!(f->map[at]&0x00ff0000u) &&
            snap_slot(f->map[at])+1u==f->source.upper_marker) {
          f->map[at]=SNAP(snap_slot(f->map[at]),0,REF_NIL);
          changed=1; break;
        }
      }
      assert(changed);
    }
    break;
  case VM_SOURCE_ARGUMENT:
    f->map[f->source_arg[0]]=SNAP(snap_slot(f->map[f->source_arg[0]]),0,REF_NIL);
    break;
  case VM_DEST_ARGUMENT:
    if (bc_b(*varg)==0u) setbc_a(varg,bc_a(*varg)+1u);
    else f->map[f->child_arg[0]]=SNAP(snap_slot(f->map[f->child_arg[0]]),0,REF_NIL);
    break;
  case VM_FRAME_SLOT:
    for (MSize i=0;i<snap->nent;i++) {
      MSize at=snap->mapofs+i;
      if ((f->map[at]&0x00ff0000u)==SNAP_FRAME &&
          snap_slot(f->map[at])==f->source.upper_marker) {
        f->map[at]=SNAP(f->source.lower_marker,0,snap_ref(f->map[at])); break;
      }
    }
    break;
  case VM_CALLMT_A: setbc_a(callmt,bc_a(*callmt)+1u); break;
  case VM_CALLMT_B: setbc_b(callmt,bc_b(*callmt)+1u); break;
  case VM_CALLMT_C: setbc_c(callmt,bc_c(*callmt)+1u); break;
  case VM_UGET_D: setbc_d(uget,bc_d(*uget)+1u); break;
  default: assert(0);
  }
}

static void vararg_admission(lua_State *L, GCtrace *T, int forward)
{
  VarargFixture f={0};
  f.live=T; f.source=find_vararg_map(T); f.nins=T->nins;
  f.pt=copy_proto(trace_startpt_acq(T));
  f.leafpt=copy_proto(funcproto(ir_kfunc(&T->ir[f.source.lower_func])));
  f.leaf=calloc(1,sizeLfunc(lj_funcL_nupvalues(
      &ir_kfunc(&T->ir[f.source.lower_func])->l)));
  f.J=calloc(1,sizeof(*f.J));
  f.ir=calloc(T->nins,sizeof(IRIns));
  f.snap=malloc(T->nsnap*sizeof(SnapShot));
  f.map=malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf&&f.J&&f.ir&&f.snap&&f.map);
  vararg_fixture_reset(&f,L);
  {
    SnapShot *snap=&f.snap[f.source.snap];
    f.source_arg[0]=f.child_arg[0]=UINT32_MAX;
    for (MSize i=0;i<snap->nent;i++) {
      MSize at=snap->mapofs+i; SnapEntry entry=f.map[at];
      if (snap_slot(entry)==f.source.callfunc+1u+LJ_FR2)
        f.source_arg[0]=at;
      if (snap_slot(entry)==f.source.childbase+bc_a(f.source.varg))
        f.child_arg[0]=at;
    }
    assert(f.source_arg[0]!=UINT32_MAX &&
           (bc_b(f.source.varg)==0u || f.child_arg[0]!=UINT32_MAX));
  }
  for (int postra=0;postra<2;postra++) {
    vararg_fixture_reset(&f,L);
    assert(vararg_fixture_admit(&f,postra));
    for (unsigned m=0;m<(forward ? VM_COUNT_FORWARD : VM_COUNT_BASE);m++) {
      int accepted;
      if (forward && m==VM_COUNT_BASE) continue;
      vararg_fixture_reset(&f,L); vararg_fixture_mutate(&f,m);
      accepted=vararg_fixture_admit(&f,postra);
      if (accepted) {
        if (m==VM_SOURCE_ARGUMENT || m==VM_DEST_ARGUMENT) {
          MSize at=m==VM_SOURCE_ARGUMENT ? f.source_arg[0] : f.child_arg[0];
          SnapEntry before=f.live->snapmap[f.live->snap[f.source.snap].mapofs+
                                           (at-f.snap[f.source.snap].mapofs)];
          fprintf(stderr,"vararg negative accepted shape=0 postra=%d mutation=%u snap=%u mapofs=%u slot=%u ref=%u before_slot=%u before_ref=%u childbase=%u\n",
                  postra,m,(unsigned)f.source.snap,(unsigned)at,
                  (unsigned)snap_slot(f.map[at]),
                  (unsigned)snap_ref(f.map[at]),
                  (unsigned)snap_slot(before),(unsigned)snap_ref(before),
                  (unsigned)f.source.childbase);
        } else {
          fprintf(stderr,"vararg negative accepted shape=0 postra=%d mutation=%u\n",
                  postra,m);
        }
      }
      assert(!accepted);
      if (postra) postra_bad++; else semantic_bad++;
    }
  }
  free(f.pt); free(f.leafpt); free(f.leaf); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

int main(void)
{
  lua_State *L=ljt_lua_newstate_openlibs();
  struct Case {
    const char *body, *runner;
    MSize nargs, requested;
    int child_map;
  };
  static const struct Case cases[]={
    {"local x,y=...; local z=x+y; return z",
     "local sum=.5; for i=1,n do sum=sum+fn(i,bias) end; return sum",2,2,1},
    {"local x,y,z=...; return x+1",
     "local sum=.5; for i=1,n do sum=sum+fn(i) end; return sum",1,3,1},
    {"local x=...; return x+1",
     "local sum=.5; for i=1,n do sum=sum+fn(i,bias,10) end; return sum",3,1,1},
    {"local x=...; return x>0",
     "local sum=.5; for i=1,n do if fn(i,bias) then sum=sum+1 end end; return sum",2,1,0},
    {"local x,y=...; return x*.5+y",
     "local sum=.5; for i=1,n do sum=sum+fn(i+.5,bias) end; return sum",2,2,0}
  };
  unsigned shape;
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(a,b) assert(a:send(true,5)); "
    "assert(select(2,b:recv(90))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "install_vararg_case=function(body,runner) "
    "leaf=assert(loadstring('return function(...) '..body..' end','@vararg-leaf'))(); "
    "leaf_oracle=assert(loadstring('return function(...) '..body..' end','@vararg-oracle-leaf'))(); "
    "local src='return function(fn,n,bias) '..runner..' end'; "
    "leaf_run=assert(loadstring(src,'@vararg-run'))(); "
    "leaf_oracle_run=assert(loadstring(src,'@vararg-oracle-run'))(); "
    "jit.off(leaf_oracle_run,true); jit.off(leaf_oracle,true); "
    "jit.flush(); jit.on(); jit.on(leaf_run,true); jit.on(leaf,true); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64') end; "
    "install_vararg_forward_case=function() "
    "local make_sum=assert(loadstring('return function(a,b,c) local x=a+b; x=x+c; return x end','@vararg-forward-sum'))(); "
    "local make_forward=assert(loadstring('return function(sum) return function(...) return sum(...) end end','@vararg-forward-factory'))(); "
    "sum_leaf=make_sum; sum_oracle=make_sum; "
    "leaf=make_forward(sum_leaf); leaf_oracle=make_forward(sum_oracle); "
    "local run='local sum=.5; for i=1,n do sum=sum+fn(i,bias,10) end; return sum'; "
    "local src='return function(fn,n,bias) '..run..' end'; "
    "leaf_run=assert(loadstring(src,'@vararg-forward-run'))(); "
    "leaf_oracle_run=assert(loadstring(src,'@vararg-forward-oracle-run'))(); "
    "jit.off(leaf_oracle_run,true); jit.off(leaf_oracle,true); jit.off(sum_oracle,true); "
    "jit.flush(); jit.on(); jit.on(leaf_run,true); jit.on(leaf,true); jit.on(sum_leaf,true); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64') end; "
    "jit.off(install_vararg_case,true)");
  for (shape=0;shape<sizeof(cases)/sizeof(cases[0]);shape++) {
    GCtrace *T=NULL;
    install_case(L,cases[shape].body,cases[shape].runner);
    assert(lj_trace_arm64_lua_leaf(global_function(L,"leaf")));
    for (unsigned attempt=0;attempt<8 && !T;attempt++) {
      lua_Number expected=invoke(L,"leaf_oracle_run","leaf_oracle",96,20);
      assert(invoke(L,"leaf_run","leaf",96,20)==expected);
      T=root_trace(L);
    }
    if (!T && shape==3) {
      puts("Lua vararg shape=3 primitive boolean result: semantic oracle passed; "
           "native publication remains closed by caller-result grammar");
      continue;
    }
    assert(T);
    if (cases[shape].child_map) {
      VarargMap map=find_vararg_map(T);
      assert(map.nargs==cases[shape].nargs &&
             map.requested==cases[shape].requested &&
             map.available==cases[shape].nargs);
    }
    native_call(L,T,64,20,cases[shape].nargs,
                cases[shape].requested,0,cases[shape].child_map);
    lua_gc(L,LUA_GCCOLLECT,0);
    native_call(L,T,24,20,cases[shape].nargs,
                cases[shape].requested,0,cases[shape].child_map);
    if (shape==0)
      native_call(L,T,8,INT32_MAX,cases[shape].nargs,
                  cases[shape].requested,1,cases[shape].child_map);
    if (shape==0) vararg_admission(L,T,0);
    printf("Lua vararg shape=%u native=%u %s GC/reuse passed\n",
           shape,shape==0 ? 3u : 2u,
           cases[shape].child_map ? "paired child map" : "root entry only");
  }
  install_forward_case(L);
  {
    GCtrace *T=NULL;
    GCfunc *forward=global_function(L,"leaf");
    assert(lj_trace_arm64_lua_leaf(forward));
    for (unsigned attempt=0;attempt<8 && !T;attempt++) {
      lua_Number expected=invoke(L,"leaf_oracle_run","leaf_oracle",96,20);
      assert(invoke(L,"leaf_run","leaf",96,20)==expected);
      T=root_trace(L);
    }
    assert(T);
    assert_forward_trace_bound(L,T,forward,global_function(L,"sum_leaf"));
    native_call(L,T,64,20,3u,3u,0,0);
    vararg_admission(L,T,1);
    ljt_lua_dostring(L,
      "leaf_saved=leaf; leaf_oracle_saved=leaf_oracle; "
      "leaf_alt=assert(loadstring('return function(a,b,c) local x=a+b; x=x+c; return x end','@vararg-forward-alt'))(); "
      "leaf_oracle_alt=assert(loadstring('return function(a,b,c) local x=a+b; x=x+c; return x end','@vararg-forward-alt-oracle'))(); "
      "jit.off(leaf_alt,true); jit.off(leaf_oracle_alt,true); "
      "leaf=leaf_alt; leaf_oracle=leaf_oracle_alt");
    native_call(L,T,24,20,3u,3u,0,0);
    assert(lj_trace_test_exit_calls()>0 &&
           lj_trace_test_first_exit_parent()==T->traceno);
    assert(root_trace(L)==T);
    ljt_lua_dostring(L,"leaf=leaf_saved; leaf_oracle=leaf_oracle_saved; leaf_saved=nil; leaf_oracle_saved=nil");
    lua_gc(L,LUA_GCCOLLECT,0);
    native_call(L,T,24,20,3u,3u,0,0);
    printf("Lua vararg-forward shape=5 native=%u exact root CALL/target guard, identity exit, GC/reuse passed\n",
           3u);
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  printf("Lua varargs native OK: entries=%u exact-child-overflow-exits=%u semantic-negative=%u postRA-negative=%u paired-frame maps, padding/truncation, primitive output, GC/reuse/peer passed\n",
         native_entries,guard_exits,semantic_bad,postra_bad);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-varargs SKIP"); return 0; }
#endif
