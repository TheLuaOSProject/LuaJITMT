/* Native pure Lua leaves, exact caller guard exits, and independent admission.
** Mutations use private copies of both prototypes, IR and snapshots. */
#include <stdio.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/proto_fixture_helpers.h"

static unsigned native_runs, semantic_bad, postra_bad, cold_retries;

static GCfunc *global_function(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1)); fn = funcV(L->top-1);
  assert(isluafunc(fn)); lua_pop(L, 1);
  return fn;
}

static lua_Number invoke(lua_State *L, const char *runner,
                         const char *leaf, int n)
{
  lua_Number result;
  void *cframe = L->cframe;
  lua_getglobal(L, runner); lua_getglobal(L, leaf); lua_pushinteger(L, n);
  ljt_lua_pcall(L, 2, 1, "Lua leaf call");
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

static SnapNo identity_exit(const GCtrace *T, IRRef *guardp)
{
  for (IRRef r = REF_FIRST; r < T->nins; r++) {
    IRIns p = ir_load_acq(&T->ir[r]);
    if (p.o == IR_EQ && p.t.irt == (IRT_FUNC|IRT_GUARD) &&
        irref_isk(p.op2) && isluafunc(ir_kfunc(&T->ir[p.op2]))) {
      for (SnapNo s = 0; s < T->nsnap; s++)
        if (T->snap[s].ref == r) {
          if (guardp) *guardp = r;
          return s;
        }
    }
  }
  assert(0 && "exact Lua identity snapshot missing"); return 0;
}

static void native_call(lua_State *L, GCtrace *T, const char *leaf,
                        const char *oracle, int n, int changed)
{
  lua_Number expected = invoke(L, "leaf_oracle", oracle, n);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L, "leaf_run", leaf, n) == expected);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno);
  if (changed) assert(lj_trace_test_first_exitno() == identity_exit(T, NULL));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  native_runs++;
}

static GCproto *copy_proto(const GCproto *pt)
{
  return ljt_proto_clone_alloc(pt);
}

typedef struct Fixture {
  GCtrace *live, copy;
  GCproto *pt, *leafpt;
  GCfunc *leaf;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef n, guard, kfunc;
  SnapNo guard_snap;
  MSize callpos, fnentry, argentry;
} Fixture;

static void fixture_reset(Fixture *f)
{
  GCtrace *T = f->live;
  GCproto *pt = trace_startpt_acq(T);
  GCfunc *leaf = ir_kfunc(&T->ir[f->kfunc]);
  MSize startpos = proto_bcpos(pt, trace_startpc_acq(T));
  ljt_proto_clone_reset(f->pt, pt);
  ljt_proto_clone_reset(f->leafpt, funcproto(leaf));
  memset(f->leaf, 0, sizeof(*f->leaf));
  memcpy(f->leaf, leaf, sizeLfunc(0));
  setmref(f->leaf->l.pc, proto_bc(f->leafpt));
  memcpy(f->ir+T->nk, T->ir+T->nk, (T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->kfunc], obj2gco(f->leaf));
  for (IRRef r=T->nk; r+1u<T->nins; r++) {
    const IRIns *constant=&T->ir[r];
    if (constant->o==IR_KNUM) {
      uintptr_t pc=(uintptr_t)T->ir[r+1u].tv.u64;
      const GCproto *owners[2]={pt,funcproto(leaf)};
      GCproto *copies[2]={f->pt,f->leafpt};
      for (unsigned p=0;p<2;p++) {
        uintptr_t lo=(uintptr_t)proto_bc(owners[p]);
        uintptr_t hi=(uintptr_t)(proto_bc(owners[p])+owners[p]->sizebc);
        if (pc>=lo && pc<=hi) {
          MSize pos=(MSize)((pc-lo)/sizeof(BCIns));
          f->ir[r+1u].tv.u64=u64ptr(&proto_bc(copies[p])[pos]);
          break;
        }
      }
      r++;
    }
  }
  memcpy(f->snap, T->snap, T->nsnap*sizeof(SnapShot));
  memcpy(f->map, T->snapmap, T->nsnapmap*sizeof(SnapEntry));
  f->copy = *T; f->copy.ir=f->ir; f->copy.snap=f->snap; f->copy.snapmap=f->map;
  trace_startpt_rel(&f->copy, f->pt);
  setmref(f->copy.startpc, &proto_bc(f->pt)[startpos]);
  proto_bc(f->pt)[startpos] = T->startins;
  for (MSize s=0; s<T->nsnap; s++) {
    SnapShot *snap=&f->snap[s];
    const BCIns *pc=snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
    const GCproto *liveleaf=funcproto(leaf);
    uintptr_t at=(uintptr_t)pc, rootlo=(uintptr_t)proto_bc(pt);
    uintptr_t leaflo=(uintptr_t)proto_bc(liveleaf);
    uint64_t pcbase;
    if (at>=rootlo && at<rootlo+pt->sizebc*sizeof(BCIns)) {
      MSize pos=(MSize)((at-rootlo)/sizeof(BCIns));
      pcbase=(u64ptr(&proto_bc(f->pt)[pos])<<8);
    } else {
      assert(at>=leaflo && at<leaflo+liveleaf->sizebc*sizeof(BCIns));
      MSize pos=(MSize)((at-leaflo)/sizeof(BCIns));
      pcbase=(u64ptr(&proto_bc(f->leafpt)[pos])<<8);
    }
    memcpy(&f->map[snap->mapofs+snap->nent], &pcbase, sizeof(pcbase));
  }
  memset(f->J,0,sizeof(*f->J));
  f->J->pt=f->pt; f->J->startpc=trace_startpc_acq(&f->copy);
  f->J->baseslot=1+LJ_FR2;
  for (IRRef r=REF_FIRST; r<f->n; r++)
    if (f->ir[r].o==IR_LOOP) f->J->loopref=r;
  f->view=(LJArm64PostRAView){0};
  f->view.ir=f->ir; f->view.owner=&f->copy; f->view.owner_private=1;
  f->view.snap=f->snap; f->view.snapmap=f->map;
  f->view.proto=f->pt; f->view.proto_bc=proto_bc(f->pt);
  f->view.startpc=trace_startpc_acq(&f->copy);
  f->view.nins=T->nins; f->view.nk=T->nk;
  f->view.nsnap=T->nsnap; f->view.nsnapmap=T->nsnapmap;
  f->view.spadjust=T->spadjust; f->view.proto_sizebc=f->pt->sizebc;
  f->view.proto_numparams=f->pt->numparams; f->view.root_topslot=T->topslot;
  f->view.startins=T->startins;
}

static int admit(Fixture *f, int allocated)
{
  if (allocated) return lj_asm_arm64_postra_admit(&f->view,NULL);
  f->copy.nins=f->n;
  return lj_asm_arm64_ir_admit(f->J,&f->copy,NULL);
}

enum {
  M_FUNC_SLOT, M_ARG_SLOT, M_CALL_ARITY, M_CALL_RESULTS, M_CALL_SLOT,
  M_CALLM, M_CALL_PC, M_SNAPSHOT_REF, M_CHILD_PC, M_CHILD_BASE, M_CHILD_FRAME,
  M_CHILD_TOP, M_GUARD_MISSING, M_GUARD_FORWARD, M_LEAF_ARITY, M_LEAF_UPVALUE,
  M_LEAF_VARARG, M_LEAF_DISABLED, M_LEAF_HEADER, M_LEAF_EFFECT, M_RESULT_BIND,
  M_LEAF_REGISTER,
  M_LEAF_CONSTANT, M_LEAF_RETURN, M_LEAF_RETURN_SLOT, M_COUNT
};

static void mutate(Fixture *f, unsigned m)
{
  SnapShot *snap=&f->snap[f->guard_snap];
  BCIns *call=&proto_bc(f->pt)[f->callpos], *leafbc=proto_bc(f->leafpt);
  uint64_t pcbase;
  switch (m) {
  case M_FUNC_SLOT: f->map[f->fnentry]=SNAP(snap_slot(f->map[f->fnentry]),0,f->kfunc); break;
  case M_ARG_SLOT: f->map[f->argentry]=SNAP(snap_slot(f->map[f->argentry]),0,f->ir[f->guard].op1); break;
  case M_CALL_ARITY: setbc_c(call,3); break;
  case M_CALL_RESULTS: setbc_b(call,bc_b(*call)+1u); break;
  case M_CALL_SLOT: setbc_a(call,bc_a(*call)-1u); break;
  case M_CALLM: setbc_op(call,BC_CALLM); break;
  case M_CALL_PC:
    pcbase=u64ptr(call+1)<<8; memcpy(&f->map[snap->mapofs+snap->nent],&pcbase,8); break;
  case M_SNAPSHOT_REF: snap->ref--; break;
  case M_CHILD_PC:
    pcbase=u64ptr(leafbc+1)<<8; memcpy(&f->map[snap->mapofs+snap->nent],&pcbase,8); break;
  case M_CHILD_BASE:
    f->map[snap->mapofs+snap->nent]|=2; break;
  case M_CHILD_FRAME: f->map[f->argentry]|=SNAP_FRAME; break;
  case M_CHILD_TOP: snap->topslot=(BCReg)(f->pt->framesize+1u+LJ_FR2); break;
  case M_GUARD_MISSING: f->ir[f->guard].t.irt=IRT_FUNC; break;
  case M_GUARD_FORWARD: f->ir[f->guard].op1=f->guard; break;
  case M_LEAF_ARITY: f->leafpt->numparams=2; break;
  case M_LEAF_UPVALUE: f->leafpt->sizeuv=1; break;
  case M_LEAF_VARARG: f->leafpt->flags|=PROTO_VARARG; break;
  case M_LEAF_DISABLED: f->leafpt->flags|=PROTO_NOJIT; break;
  case M_LEAF_HEADER: setbc_op(&leafbc[0],BC_IFUNCF); break;
  case M_LEAF_EFFECT: leafbc[1]=BCINS_AD(BC_USETV,0,0); break;
  case M_RESULT_BIND: {
    if (bc_b(*call)==3u) {
      int found=0;
      for (MSize pos=f->callpos+1u; pos<f->pt->sizebc; pos++) {
        BCIns *ins=&proto_bc(f->pt)[pos];
        if (bc_op(*ins)==BC_CGET && bc_d(*ins)==bc_a(*call)+1u) {
          setbc_d(ins,bc_a(*call)); found=1; break;
        }
        if (bc_op(*ins)==BC_FORL || bc_op(*ins)==BC_JFORL) break;
      }
      assert(found);
    } else {
      setbc_b(call,3u);
    }
    break;
  }
  case M_LEAF_REGISTER: setbc_d(&leafbc[1],f->leafpt->framesize); break;
  case M_LEAF_CONSTANT: {
    BCIns *constant=&leafbc[2];
    if (bc_op(*constant)==BC_KNUM) setbc_d(constant,f->leafpt->sizekn);
    else setbc_c(constant,f->leafpt->sizekn);
    break;
  }
  case M_LEAF_RETURN: {
    BCIns *ret=&leafbc[f->leafpt->sizebc-1];
    setbc_op(ret,bc_op(*ret)==BC_RET ? BC_RET1 : BC_RET0);
    break;
  }
  case M_LEAF_RETURN_SLOT: {
    BCIns *ret=&leafbc[f->leafpt->sizebc-1];
    setbc_a(ret,bc_op(*ret)==BC_RET ? f->leafpt->framesize-1u :
                                      f->leafpt->framesize);
    break;
  }
  default: assert(0);
  }
}

static void admission(GCtrace *T)
{
  Fixture f={0};
  f.live=T; f.guard_snap=identity_exit(T,&f.guard); f.kfunc=T->ir[f.guard].op2;
  f.n=T->nins;
  if (T->ir[f.n-1].o==IR_NOP) f.n--;
  else while (T->ir[f.n-1].o==IR_RENAME) f.n--;
  f.pt=copy_proto(trace_startpt_acq(T));
  f.leafpt=copy_proto(funcproto(ir_kfunc(&T->ir[f.kfunc])));
  f.leaf=calloc(1,sizeof(*f.leaf)); f.J=calloc(1,sizeof(*f.J));
  f.ir=calloc(T->nins,sizeof(IRIns)); f.snap=malloc(T->nsnap*sizeof(SnapShot));
  f.map=malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf&&f.J&&f.ir&&f.snap&&f.map);
  {
    SnapShot *snap=&T->snap[f.guard_snap];
    const BCIns *call=snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
    MSize slot=1u+LJ_FR2+bc_a(*call);
    {
      BCIns ret=proto_bc(f.leafpt)[f.leafpt->sizebc-1u];
      unsigned nresults=bc_op(ret)==BC_RET ? 2u : 1u;
      assert(bc_op(*call)==BC_CALL && bc_b(*call)==nresults+1u &&
             bc_c(*call)==2);
    }
    f.callpos=proto_bcpos(trace_startpt_acq(T),call);
    f.fnentry=f.argentry=UINT32_MAX;
    for (MSize i=0;i<snap->nent;i++) {
      MSize at=snap->mapofs+i;
      if (snap_slot(T->snapmap[at])==slot) f.fnentry=at;
      if (snap_slot(T->snapmap[at])==slot+1u+LJ_FR2) f.argentry=at;
    }
    assert(f.fnentry!=UINT32_MAX && f.argentry!=UINT32_MAX);
  }
  for (int allocated=0;allocated<2;allocated++) {
    for (unsigned m=0;m<M_COUNT;m++) {
      fixture_reset(&f);
      if (!admit(&f,allocated)) {
        LJArm64IRReject reject = {0};
        if (!allocated) (void)lj_asm_arm64_ir_admit(f.J,&f.copy,&reject);
        fprintf(stderr,"leaf baseline rejected postra=%d mutation=%u header=%u "
                "reason=%u ref=%u op=%u detail=%u\n",allocated,m,
                (unsigned)bc_op(proto_bc(f.leafpt)[0]),
                (unsigned)reject.reason,(unsigned)reject.ref,
                (unsigned)reject.op,(unsigned)reject.detail);
        assert(0);
      }
      fixture_reset(&f); mutate(&f,m);
      if (admit(&f,allocated)) {
        assert(0);
      }
      if (allocated) postra_bad++; else semantic_bad++;
    }
  }
  fixture_reset(&f); assert(admit(&f,1));
  f.ir[f.ir[f.guard].op1].r=RID_D0;
  assert(!admit(&f,1)); postra_bad++;
  fixture_reset(&f); assert(admit(&f,1));
  f.ir[f.ir[f.guard].op1].s=254;
  assert(!admit(&f,1)); postra_bad++;
  free(f.pt); free(f.leafpt); free(f.leaf); free(f.J); free(f.ir); free(f.snap); free(f.map);
}

/* The open-result CALLM source has a guard-free producer CALL. Its semantic
** and allocated proofs must replay that producer from the exact source guard
** and bind all returned values into this CALLM's own argument map. */
static void admission_open_callm(GCtrace *T, unsigned nresults)
{
  Fixture f={0};
  MSize callmpos=UINT32_MAX, childslot=UINT32_MAX;
  MSize producersnap=UINT32_MAX;
  unsigned found_callmap=0;
  f.live=T; f.guard_snap=identity_exit(T,&f.guard); f.kfunc=T->ir[f.guard].op2;
  f.n=T->nins;
  while (f.n>T->nk &&
         (T->ir[f.n-1u].o==IR_NOP || T->ir[f.n-1u].o==IR_RENAME)) f.n--;
  f.pt=copy_proto(trace_startpt_acq(T));
  f.leafpt=copy_proto(funcproto(ir_kfunc(&T->ir[f.kfunc])));
  f.leaf=calloc(1,sizeof(*f.leaf)); f.J=calloc(1,sizeof(*f.J));
  f.ir=calloc(T->nins,sizeof(IRIns)); f.snap=malloc(T->nsnap*sizeof(SnapShot));
  f.map=malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf && f.J && f.ir && f.snap && f.map);
  fixture_reset(&f);
  for (MSize p=1; p+1u<f.pt->sizebc; p++)
    if (bc_op(proto_bc(f.pt)[p])==BC_CALLM) {
      assert(callmpos==UINT32_MAX); callmpos=p;
    }
  assert(nresults>=1u && nresults<=3u && callmpos!=UINT32_MAX &&
         bc_b(proto_bc(f.pt)[callmpos])==2u &&
         bc_c(proto_bc(f.pt)[callmpos])==0u &&
         bc_op(proto_bc(f.pt)[callmpos-1u])==BC_CALL &&
         bc_b(proto_bc(f.pt)[callmpos-1u])==0u &&
         bc_a(proto_bc(f.pt)[callmpos-1u])==
           bc_a(proto_bc(f.pt)[callmpos])+1u+LJ_FR2);
  for (SnapNo s=0; s<T->nsnap; s++) {
    SnapShot *snap=&f.snap[s];
    const BCIns *pc=snap_pc(&f.map[snap->mapofs+snap->nent]);
    if (snap->ref<REF_FIRST || snap->ref>=T->nins) continue;
    IRIns guard=f.ir[snap->ref];
    if (pc==proto_bc(f.pt)+callmpos && guard.o==IR_EQ &&
        guard.t.irt==(IRT_FUNC|IRT_GUARD)) {
      MSize slot=1u+LJ_FR2+bc_a(proto_bc(f.pt)[callmpos])+
                 1u+LJ_FR2+nresults-1u;
      for (MSize i=0; i<snap->nent; i++) {
        MSize at=snap->mapofs+i;
        if (snap_slot(f.map[at])==slot && !(f.map[at]&0x00ff0000u)) {
          childslot=at; found_callmap++;
        }
      }
    }
    if (pc==proto_bc(f.pt)+callmpos-1u && guard.o==IR_EQ &&
        guard.t.irt==(IRT_FUNC|IRT_GUARD)) {
      assert(producersnap==UINT32_MAX);
      producersnap=s;
    }
  }
  assert(found_callmap==1u && childslot!=UINT32_MAX &&
         producersnap!=UINT32_MAX);
  for (int postra=0; postra<2; postra++) {
    fixture_reset(&f); assert(admit(&f,postra));
    proto_bc(f.pt)[callmpos]=BCINS_ABC(BC_CALLM,
      bc_a(proto_bc(f.pt)[callmpos]),3u,0u);
    assert(!admit(&f,postra));
    fixture_reset(&f); assert(admit(&f,postra));
    f.map[childslot]=SNAP(snap_slot(f.map[childslot]),0,REF_NIL);
    assert(!admit(&f,postra));
    fixture_reset(&f); assert(admit(&f,postra));
    {
      SnapShot *snap=&f.snap[producersnap];
      uint64_t wrongpc=u64ptr(proto_bc(f.pt)+callmpos)<<8;
      memcpy(&f.map[snap->mapofs+snap->nent],&wrongpc,sizeof(wrongpc));
    }
    assert(!admit(&f,postra));
    if (postra) postra_bad+=3u; else semantic_bad+=3u;
  }
  free(f.pt); free(f.leafpt); free(f.leaf); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

int main(void)
{
  lua_State *L=ljt_lua_newstate_openlibs();
  const char *bodies[]={"return x*.5+1.25","return 1.25-x*.25","return .5/x+1.25","return x,0.25"};
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(a,b) assert(a:send(true,5)); assert(select(2,b:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "function install_leaf(body,multi) local r=multi and 'return function(fn,n) local sum=.5; for i=1,n do local a,b=fn(i+.5); sum=sum+a+b end; return sum end' or 'return function(fn,n) local sum=.5; for i=1,n do sum=sum+fn(i+.5) end; return sum end'; "
    "leaf_run=assert(loadstring(r,'@leaf-run'))(); leaf_oracle=assert(loadstring(r,'@leaf-oracle'))(); "
    "leaf_a=assert(loadstring('return function(x) '..body..' end','@leaf-a'))(); "
    "leaf_oracle_a=assert(loadstring('return function(x) '..body..' end','@leaf-oracle-a'))(); "
    "leaf_b=assert(loadstring('return function(x) return x*.125+2.5 end','@leaf-b'))(); "
    "leaf_oracle_b=assert(loadstring('return function(x) return x*.125+2.5 end','@leaf-oracle-b'))(); "
    "jit.off(leaf_oracle,true); jit.off(leaf_oracle_a,true); jit.off(leaf_oracle_b,true); "
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64') end; jit.off(install_leaf,true)");
  for (unsigned shape=0;shape<sizeof(bodies)/sizeof(bodies[0]);shape++) {
    GCtrace *T=NULL;
    int multi=shape==3;
    lua_getglobal(L,"install_leaf"); lua_pushstring(L,bodies[shape]); lua_pushboolean(L,multi); ljt_lua_pcall(L,2,0,"install leaf");
    if (multi) {
      ljt_lua_dostring(L,"leaf_b=assert(loadstring('return function(x) return x*.125+2.5,0.125 end','@leaf-b'))(); leaf_oracle_b=assert(loadstring('return function(x) return x*.125+2.5,0.125 end','@leaf-oracle-b'))(); jit.off(leaf_oracle_b,true)");
    }
    assert(lj_trace_arm64_lua_leaf(global_function(L,"leaf_a")));
    for (unsigned attempt=0;attempt<8 && !T;attempt++) {
      lua_Number expected=invoke(L,"leaf_oracle","leaf_oracle_a",96);
      assert(invoke(L,"leaf_run","leaf_a",96)==expected);
      T=root_trace(L); if (attempt) cold_retries++;
    }
    assert(T);
    native_call(L,T,"leaf_a","leaf_oracle_a",96,0);
    native_call(L,T,"leaf_b","leaf_oracle_b",48,1);
    native_call(L,T,"leaf_a","leaf_oracle_a",24,0);
    lua_gc(L,LUA_GCCOLLECT,0);
    native_call(L,T,"leaf_a","leaf_oracle_a",64,0);
    admission(T);
    printf("Lua leaf shape=%u native=4 exact identity exit and same-trace reuse passed\n",shape);
  }
  ljt_lua_dostring(L,
    "install_open_case=function(producer,consumer) "
      "leaf_a=assert(loadstring('return function(x) '..producer..' end',"
        "'@open-callm-values'))(); "
      "leaf_oracle_a=assert(loadstring('return function(x) '..producer..' end',"
        "'@open-callm-values-oracle'))(); "
      "local src='return function(fn,n) local add=function(a,b,c) '..consumer..' end; "
        "local total=.5; for i=1,n do total=total+add(fn(i+.5)) end; "
        "return total end'; "
      "leaf_run=assert(loadstring(src,'@open-callm-run'))(); "
      "leaf_oracle=assert(loadstring(src,'@open-callm-oracle'))(); "
      "leaf_run_clone=assert(loadstring(src,'@open-callm-run-clone'))(); "
      "jit.off(leaf_oracle,true); jit.off(leaf_oracle_a,true); "
      "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64') end; "
    "jit.off(install_open_case,true)");
  {
    const char *producer[] = {"return x", "return x,.25", "return x,.25,.5"};
    const char *consumer[] = {"return a+1", "return a+b", "return a+b+c"};
    {
    unsigned nresults=3u;
    GCtrace *T=NULL;
    uint32_t rootstarts=lj_trace_test_root_entry_startins_calls();
    lua_getglobal(L,"install_open_case");
    lua_pushstring(L,producer[nresults-1u]);
    lua_pushstring(L,consumer[nresults-1u]);
    ljt_lua_pcall(L,2,0,"install open CALLM case");
    for (unsigned attempt=0; attempt<8 && !T; attempt++) {
      lua_Number expected=invoke(L,"leaf_oracle","leaf_oracle_a",96);
      assert(invoke(L,"leaf_run","leaf_a",96)==expected);
      T=root_trace(L);
    }
    if (!T) {
      GCproto *runpt=funcproto(global_function(L,"leaf_run"));
      GCproto *leafpt=funcproto(global_function(L,"leaf_a"));
      fprintf(stderr,"open CALLM result count=%u no root aborts=%u last=%u "
        "root-starts=%u jitflags=%08x run-size=%u leaf-size=%u leaf-ret-op=%u leaf-ret-d=%u\n",
        nresults,(unsigned)lj_trace_test_abort_count(),
        (unsigned)lj_trace_test_last_abort_error(),
        (unsigned)(lj_trace_test_root_entry_startins_calls()-rootstarts),
        (unsigned)L2J(L)->flags,
        (unsigned)runpt->sizebc,(unsigned)leafpt->sizebc,
        (unsigned)bc_op(proto_bc(leafpt)[leafpt->sizebc-1u]),
        (unsigned)bc_d(proto_bc(leafpt)[leafpt->sizebc-1u]));
      for (MSize p=0; p<runpt->sizebc; p++) {
        BCIns ins=proto_bc(runpt)[p];
        fprintf(stderr,"open rootbc pc=%u op=%u a=%u b=%u c=%u\n",
          (unsigned)p,(unsigned)bc_op(ins),(unsigned)bc_a(ins),
          (unsigned)bc_b(ins),(unsigned)bc_c(ins));
      }
    }
    assert(T);
    native_call(L,T,"leaf_a","leaf_oracle_a",96,0);
    lua_gc(L,LUA_GCCOLLECT,0);
    native_call(L,T,"leaf_a","leaf_oracle_a",64,0);
    admission_open_callm(T,nresults);
    printf("Lua open-result CALLM results=%u: native root, GC reuse and independent source/map refusals passed\n",
           nresults);
    }
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  printf("Lua leaf calls OK: native=%u semantic-negative=%u postRA-negative=%u cold-retries=%u\n",
         native_runs,semantic_bad,postra_bad,cold_retries);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-leaf-calls SKIP"); return 0; }
#endif
